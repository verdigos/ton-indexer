"""Field-level parity between the DeDust CPMM V2 swap matchers and
specs_decl/dedust_v2_swap.mch.

Both legacy matchers (DedustV2SwapBlockMatcher, DedustV2FailedSwapBlockMatcher)
run as one group against both spec matchers, because the spec's success/failure
split follows the reference's: the success matcher runs first and the failure
matcher only fires where it bailed.

Assertion depth is the usual one: btype, deep `data` equality (value + exact
type), failed/broken flags, consumed event-node set. `value_flow` is NOT
compared — declarative matchers no longer populate it (the `flow` statement was
removed) while the legacy builders still do.

Corpus fixtures, one per shape the spec has to get right:
- 2UJH7bBZDI4vmFG7O7HgraaQQF0BfjS2Eg5JaY62_Zo=  jetton-in single hop, TON out
    (the live A/B-gap fixture; also the YAML case "dedust v2 swap (ab-gap)").
- OV5zvYSKMoiFa3iYtadO55uo3w5hdiBtXRl7Qc-7SiA=  TON-in (PayNative) single hop,
    jetton out, inside a trace that also carries a dedust V1 swap.
- APLmhF6A2x2HJkhGrBOB86fUizlonodaK7bUPNGDOIU=  2 hops through a NATIVE
    intermediate carrier (jetton -> TON -> jetton).
- MZQz_qHLV8OiohUw3-mjpA7TihnbDKFGE0i7LUUsuVk=  3 hops through JETTON
    intermediate carriers — the shape that needs the `payload_opcode` where
    clauses (an intermediate carrier must not be taken for the out leg) and the
    frontier-driven walk.
- 0iATkyOyvTvNjrVp0CbQo4RDIo7wljsohpVizWVHE1A=  TON-in slippage refund
    (Payout exit_code != 0), destination_asset resolved from the pool lookup.
- 1fOLTYG-BKmjzSDbL1SIsXYO2lizqWyluPgmgfAm2Wg=  jetton-in slippage refund
    (same asset back to the sender).
- Ou-hXwfHtNeTs97mBY3LVCmlnsoySA7hJXeoZSfOAao=  TON-in refund whose pool is NOT
    in the trace's interfaces: the lookup misses and destination_asset is null.

Corpus gaps (no recording per task rules): no swap whose pool transaction
aborted (the `failed` flag stays False on every corpus swap), and no route that
fails mid-hop after a successful first hop (the "out leg still carries a
SwapPayload" reject).
"""
from __future__ import annotations

from pathlib import Path

import pytest

from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.dedust_v2.swaps import (
    DedustV2FailedSwapBlockMatcher,
    DedustV2SwapBlockMatcher,
)
from indexer.events.blocks.jettons import JettonTransferBlockMatcher
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
import indexer.events.mch as mch
from indexer.events.mch import compile_file
from indexer.events.mch.builders import jetton_decl_registries
from indexer.events.mch.builders.dedust_v2 import dedust_v2_swap_registries
from indexer.events.mch.ir_emit import emit_ir

DEDUST_V2_MCH = Path(mch.__file__).parent / "specs_decl" / "dedust_v2_swap.mch"
PTON_MCH = Path(mch.__file__).parent / "specs_decl" / "pton_transfer.mch"

try:
    from indexer.events import mch_ir as _mch_ir

    _HAVE_IR = hasattr(_mch_ir, "load_artifact") and hasattr(_mch_ir, "build_matchers")
except Exception:  # pragma: no cover - import error path
    _mch_ir = None
    _HAVE_IR = False

_SKIP_REASON = "indexer.events.mch_ir not importable; the IR engine is the DSL runtime"


def _prerequisites() -> list[BlockMatcher]:
    """Tree preparation both legs share: the jetton and pTON transfer blocks the
    swap pattern binds as `btype jetton_transfer` (spec-built pton, like every
    other DEX port since the pton wave)."""
    compiled = compile_file(PTON_MCH, jetton_decl_registries())
    art = _mch_ir.load_artifact(emit_ir(compiled, [PTON_MCH.name]))
    (pton,) = _mch_ir.build_matchers(art, jetton_decl_registries())
    return [JettonTransferBlockMatcher(), pton]


def _dsl_matchers() -> list[BlockMatcher]:
    if not _HAVE_IR:
        pytest.skip(_SKIP_REASON)
    compiled = compile_file(DEDUST_V2_MCH, dedust_v2_swap_registries())
    art = _mch_ir.load_artifact(emit_ir(compiled, [DEDUST_V2_MCH.name]))
    return _mch_ir.build_matchers(art, dedust_v2_swap_registries())


def _build_block_tree(trace) -> Block:
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matchers(matchers: list[BlockMatcher], trace) -> list[Block]:
    root = _build_block_tree(trace)
    for pre in _prerequisites():
        for b in root.bfs_iter():
            if b.parent is None:
                await pre.try_build(b)
    produced: list[Block] = []
    for matcher in matchers:
        for b in root.bfs_iter():
            if b.btype == "root" or b.parent is not None:
                continue
            result = await matcher.try_build(b)
            if result is not None:
                produced.extend(result)
    return [b for b in produced
            if b.btype == "jetton_swap" and b.data.get("dex") == "dedust_v2"]


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
    assert len(hw_blocks) > 0, "legacy matchers produced no dedust_v2 blocks - bad fixture"
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
    hw = await _run_matchers(
        [DedustV2SwapBlockMatcher(), DedustV2FailedSwapBlockMatcher()], trace)
    dsl = await _run_matchers(_dsl_matchers(), trace)
    _assert_field_parity(dsl, hw)
    return dsl, hw


@pytest.mark.asyncio
@pytest.mark.trace_file("2UJH7bBZDI4vmFG7O7HgraaQQF0BfjS2Eg5JaY62_Zo=.lz4")
async def test_dedust_v2_parity_jetton_in_single_hop(trace_data):
    """Jetton-in single hop, TON out: peer_swaps stays [] and min_out_amount
    comes from the carrier's forward payload (the PayJetton wrapper)."""
    trace, _ = trace_data
    dsl, hw = await _run_parity(trace)
    assert hw[0].data["peer_swaps"] == []
    assert dsl[0].data["min_out_amount"] is not None, "forward-payload min_out lost"


@pytest.mark.asyncio
@pytest.mark.trace_file("OV5zvYSKMoiFa3iYtadO55uo3w5hdiBtXRl7Qc-7SiA=.lz4")
async def test_dedust_v2_parity_ton_in_single_hop(trace_data):
    """TON-in (PayNative) single hop: the swap payload comes out of the parsed
    body's payment_payload ref, and the dedust V1 swap in the same trace stays
    out of the match."""
    trace, _ = trace_data
    _, hw = await _run_parity(trace)
    assert hw[0].data["source_asset"].is_ton


@pytest.mark.asyncio
@pytest.mark.trace_file("APLmhF6A2x2HJkhGrBOB86fUizlonodaK7bUPNGDOIU=.lz4")
async def test_dedust_v2_parity_multihop_native_carrier(trace_data):
    """2 hops through a NATIVE intermediate carrier: the peer-swap assets come
    from the carriers, so the middle leg must read TON."""
    trace, _ = trace_data
    _, hw = await _run_parity(trace)
    peers = hw[0].data["peer_swaps"]
    assert len(peers) == 2, "fixture no longer exercises the intended hop count"
    assert peers[0]["out"]["asset"].is_ton, "fixture no longer has a TON mid-hop"


@pytest.mark.asyncio
@pytest.mark.trace_file("MZQz_qHLV8OiohUw3-mjpA7TihnbDKFGE0i7LUUsuVk=.lz4")
async def test_dedust_v2_parity_multihop_jetton_carriers(trace_data):
    """3 hops through JETTON intermediate carriers — the shape that fails if the
    walk mistakes an intermediate carrier for the out leg."""
    trace, _ = trace_data
    _, hw = await _run_parity(trace)
    assert len(hw[0].data["peer_swaps"]) == 3, (
        "fixture no longer exercises the intended hop count"
    )


@pytest.mark.asyncio
@pytest.mark.trace_file("0iATkyOyvTvNjrVp0CbQo4RDIo7wljsohpVizWVHE1A=.lz4")
async def test_dedust_v2_parity_failed_ton_in(trace_data):
    """TON-in slippage refund: Payout with a non-zero exit code back to the
    sender, destination_asset from the pool lookup."""
    trace, _ = trace_data
    _, hw = await _run_parity(trace)
    assert hw[0].failed is True
    assert hw[0].data["destination_asset"] is not None, "pool lookup no longer resolves"
    assert hw[0].data["dex_outgoing_transfer"]["amount"].value == 0


@pytest.mark.asyncio
@pytest.mark.trace_file("1fOLTYG-BKmjzSDbL1SIsXYO2lizqWyluPgmgfAm2Wg=.lz4")
async def test_dedust_v2_parity_failed_jetton_in(trace_data):
    """Jetton-in slippage refund: the refund transfer carries the SAME asset
    back to the carrier's sender."""
    trace, _ = trace_data
    _, hw = await _run_parity(trace)
    assert hw[0].failed is True
    assert not hw[0].data["source_asset"].is_ton


@pytest.mark.asyncio
@pytest.mark.trace_file("Ou-hXwfHtNeTs97mBY3LVCmlnsoySA7hJXeoZSfOAao=.lz4")
async def test_dedust_v2_parity_failed_pool_lookup_miss(trace_data):
    """The refunded pool is absent from the trace's interfaces: the lookup
    misses and destination_asset stays null on both legs (the null-safe arm of
    the pool-asset pick)."""
    trace, _ = trace_data
    dsl, hw = await _run_parity(trace)
    assert hw[0].data["destination_asset"] is None
    assert dsl[0].data["dex_outgoing_transfer"]["asset"] is None
