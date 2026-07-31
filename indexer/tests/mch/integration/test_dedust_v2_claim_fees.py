"""Field-level parity between DedustV2ClaimFeesBlockMatcher and
specs/dedust_v2_claim_fees.mch.

Assertion depth is the usual one: btype, deep `data` equality (value + exact
type), failed/broken flags, consumed event-node set. `value_flow` is NOT compared
(declarative matchers no longer populate it).

The point of interest is the SLOT ASSIGNMENT: legacy matches each payout leg to
the x or y slot by exact amount against PayoutPositionFees, first-match-wins, and
the spec unrolls that into four booleans. The five corpus fixtures cover every
arm the unrolling has:

- 846TnKmLxd4Otdbr4cMnvoqFJmzDT09IFTrGC9bpmes=  x_fees == 0 with ONE TON payout
    that matches y_fees — the leg lands in slot y, slot x stays empty (the
    "first branch tested, second branch taken" path, and the one that makes the
    `not x_is_1` guard on y_is_1 load-bearing).
- t3w52NsDV64FlPrPzP-KiGEZDz5C_4YLG9HaIDM9mpM=  TON leg + jetton leg, one per
    slot (the mixed-arm case: ton_asset() on one side, the prebuilt
    jetton_transfer's data on the other).
- fjQUb23kT00oQ91fTrD48MAYii2jG9XD_bft1iLtzJc=  two jetton legs.
- cymBOUsfra7ZhoecaxwLyvSZi3EhpOFmvTPTSaKHfeA=  the second two-leg trace, kept
    because it is the only one the YAML cases do not already pin.
- tJchcmap3KKyjSw6GJ2YpZ4OkyN-DwvoD2u_KQh164Q=  BOTH fees zero, so no payout leg
    forms at all: every asset/wallet field must come out null, which is where a
    naive `??` chain over an unbound capture would have leaked a TON asset.
"""
from __future__ import annotations

from pathlib import Path

import pytest

from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.dedust_v2.claims import DedustV2ClaimFeesBlockMatcher
from indexer.events.blocks.jettons import JettonTransferBlockMatcher
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
import indexer.events.mch as mch
from indexer.events.mch import compile_file
from indexer.events.mch.builders import jetton_decl_registries
from indexer.events.mch.builders.dedust_v2 import dedust_v2_claim_fees_registries
from indexer.events.mch.ir_emit import emit_ir
import indexer.events.mch.specs as specs_pkg

CLAIM_FEES_MCH = Path(specs_pkg.__file__).parent / "dedust_v2_claim_fees.mch"
PTON_MCH = Path(mch.__file__).parent / "specs_decl" / "pton_transfer.mch"

try:
    from indexer.events import mch_ir as _mch_ir

    _HAVE_IR = hasattr(_mch_ir, "load_artifact") and hasattr(_mch_ir, "build_matchers")
except Exception:  # pragma: no cover - import error path
    _mch_ir = None
    _HAVE_IR = False

_SKIP_REASON = "indexer.events.mch_ir not importable; the IR engine is the DSL runtime"


def _prerequisites() -> list[BlockMatcher]:
    """The jetton and pTON transfer blocks the payout legs bind as
    `btype jetton_transfer` (spec-built pton, like every other DEX port)."""
    compiled = compile_file(PTON_MCH, jetton_decl_registries())
    art = _mch_ir.load_artifact(emit_ir(compiled, [PTON_MCH.name]))
    (pton,) = _mch_ir.build_matchers(art, jetton_decl_registries())
    return [JettonTransferBlockMatcher(), pton]


def _dsl_matcher() -> BlockMatcher:
    if not _HAVE_IR:
        pytest.skip(_SKIP_REASON)
    compiled = compile_file(CLAIM_FEES_MCH, dedust_v2_claim_fees_registries())
    art = _mch_ir.load_artifact(emit_ir(compiled, [CLAIM_FEES_MCH.name]))
    (matcher,) = _mch_ir.build_matchers(art, dedust_v2_claim_fees_registries())
    return matcher


def _build_block_tree(trace) -> Block:
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matcher(matcher: BlockMatcher, trace) -> list[Block]:
    root = _build_block_tree(trace)
    for pre in _prerequisites():
        for b in root.bfs_iter():
            if b.parent is None:
                await pre.try_build(b)
    produced: list[Block] = []
    for b in root.bfs_iter():
        if b.btype == "root" or b.parent is not None:
            continue
        result = await matcher.try_build(b)
        if result is not None:
            produced.extend(result)
    return [b for b in produced if b.btype == "dedust_v2_claim_fees"]


def _values_equal(a, b) -> bool:
    if a is None and b is None:
        return True
    if type(a) is not type(b):
        return False
    return bool(a == b)


def _node_hashes(block: Block) -> list[str]:
    return sorted(n.message.tx_hash for n in block.event_nodes)


def _sort_key(block: Block):
    return (block.min_lt, tuple(_node_hashes(block)))


def _assert_field_parity(dsl_blocks: list[Block], hw_blocks: list[Block]) -> None:
    assert len(hw_blocks) > 0, "legacy matcher produced no claim_fees blocks - bad fixture"
    assert len(dsl_blocks) == len(hw_blocks), (
        f"count mismatch: dsl={len(dsl_blocks)} legacy={len(hw_blocks)}"
    )
    for d, h in zip(sorted(dsl_blocks, key=_sort_key), sorted(hw_blocks, key=_sort_key)):
        assert d.failed == h.failed, f"failed flag: dsl={d.failed} legacy={h.failed}"
        assert d.broken == h.broken, f"broken flag: dsl={d.broken} legacy={h.broken}"
        assert set(d.data.keys()) == set(h.data.keys()), (
            f"data keys: dsl={sorted(d.data)} legacy={sorted(h.data)}"
        )
        for key in sorted(h.data):
            dv, hv = d.data[key], h.data[key]
            assert _values_equal(dv, hv), (
                f"data[{key!r}]: dsl={dv!r} ({type(dv).__name__}) "
                f"legacy={hv!r} ({type(hv).__name__})"
            )
        assert _node_hashes(d) == _node_hashes(h), (
            f"consumed event nodes: dsl={_node_hashes(d)} legacy={_node_hashes(h)}"
        )


async def _run_parity(trace) -> tuple[list[Block], list[Block]]:
    hw = await _run_matcher(DedustV2ClaimFeesBlockMatcher(), trace)
    dsl = await _run_matcher(_dsl_matcher(), trace)
    _assert_field_parity(dsl, hw)
    return dsl, hw


@pytest.mark.asyncio
@pytest.mark.trace_file("846TnKmLxd4Otdbr4cMnvoqFJmzDT09IFTrGC9bpmes=.lz4")
async def test_claim_fees_single_ton_leg_lands_in_y(trace_data):
    trace, _ = trace_data
    dsl, _hw = await _run_parity(trace)
    data = dsl[0].data
    assert data["amount_x"].value == 0
    assert data["asset_x"] is None, "the only leg must not be claimed by slot x"
    assert data["asset_y"] is not None and data["asset_y"].is_ton


@pytest.mark.asyncio
@pytest.mark.trace_file("t3w52NsDV64FlPrPzP-KiGEZDz5C_4YLG9HaIDM9mpM=.lz4")
async def test_claim_fees_ton_and_jetton_legs(trace_data):
    trace, _ = trace_data
    dsl, _hw = await _run_parity(trace)
    data = dsl[0].data
    assert data["asset_x"].is_ton, "TON leg lost its slot"
    assert not data["asset_y"].is_ton
    assert data["dex_jetton_wallet_y"] is not None


@pytest.mark.asyncio
@pytest.mark.trace_file("fjQUb23kT00oQ91fTrD48MAYii2jG9XD_bft1iLtzJc=.lz4")
async def test_claim_fees_two_jetton_legs(trace_data):
    trace, _ = trace_data
    dsl, _hw = await _run_parity(trace)
    data = dsl[0].data
    assert data["asset_x"] is not None and data["asset_y"] is not None
    assert data["user_wallet_x"] != data["user_wallet_y"]


@pytest.mark.asyncio
@pytest.mark.trace_file("cymBOUsfra7ZhoecaxwLyvSZi3EhpOFmvTPTSaKHfeA=.lz4")
async def test_claim_fees_second_two_leg_trace(trace_data):
    trace, _ = trace_data
    await _run_parity(trace)


@pytest.mark.asyncio
@pytest.mark.trace_file("tJchcmap3KKyjSw6GJ2YpZ4OkyN-DwvoD2u_KQh164Q=.lz4")
async def test_claim_fees_zero_fees_no_legs(trace_data):
    trace, _ = trace_data
    dsl, _hw = await _run_parity(trace)
    data = dsl[0].data
    assert data["amount_x"].value == 0 and data["amount_y"].value == 0
    for key in ("asset_x", "asset_y", "user_wallet_x", "user_wallet_y",
                "dex_wallet_x", "dex_wallet_y",
                "dex_jetton_wallet_x", "dex_jetton_wallet_y"):
        assert data[key] is None, f"{key} leaked a value with no payout leg"
