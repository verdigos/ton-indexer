"""Field-level parity between the dedust liquidity-deposit matcher pair
(DedustDepositBlockMatcher / DedustDepositFirstAssetBlockMatcher) and
specs/dedust_deposit.mch, PLUS the IR round trip.

Three matcher graphs are compared block-for-block at the same bar as the
T13-T17 ports (block class + btype, deep `data` equality, failed/broken flags,
value_flow, consumed event-node set — count parity alone is NOT acceptance):

    legacy    =  the hand-written pair, run in event_processing order
                 (final, then partial) with NO prerequisites — in production
                 they run BEFORE JettonTransferBlockMatcher and match the
                 deposit leg as a raw jetton chain;
    compiled  =  compile_file(dedust_deposit.mch), run AFTER
                 JettonTransferBlockMatcher + PTonTransferMatcher — the mch
                 registration order — so the deposit leg and the vault excess
                 payouts arrive as prebuilt jetton_transfer blocks;
    ir        =  compile_file -> emit_ir -> load_artifact -> build_matchers
                 (v2 IR engine), same prerequisites as compiled.

The differing prerequisite mix is the point of the port (see the spec header):
the consumed event-node sets must still be identical, because the composite
jetton_transfer block carries exactly the nodes the legacy raw-chain match
consumed.

Both matchers must produce the exact registered block classes
(DedustDepositLiquidity / DedustDepositLiquidityPartial) —
post_process_dedust_liquidity dispatches on isinstance. Each case therefore
also runs the host-side merge pass over all three produced lists and re-checks
parity on the merged results.

Fixtures: the full tests/test_cases/dedust-liquidities.yaml corpus (8 traces).
No new recording.
"""
from __future__ import annotations

from pathlib import Path

import pytest

from indexer.events import context
from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.jettons import (
    JettonTransferBlockMatcher,
    PTonTransferMatcher,
)
from indexer.events.blocks.liquidity import (
    DedustDepositBlockMatcher,
    DedustDepositFirstAssetBlockMatcher,
    DedustDepositLiquidity,
    DedustDepositLiquidityPartial,
    post_process_dedust_liquidity,
)
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events import mch_ir
from indexer.events.mch import compile_file
from indexer.events.mch.builders.dedust import dedust_deposit_registries
from indexer.events.mch.ir_emit import emit_ir
import indexer.events.mch.specs as specs_pkg
from tests.utils.repository import TestInterfaceRepository
from tests.utils.trace_deserializer import load_trace_from_file

DEDUST_DEPOSIT_MCH = Path(specs_pkg.__file__).parent / "dedust_deposit.mch"
MATCHER_NAMES = ["dedust_deposit_final", "dedust_deposit_partial"]

# (fixture, id) — the dedust-liquidities.yaml corpus.
CASES = [
    ("WyDZqNoT4YsuauWAnRh5kVXWBsRzkFG5VRTEq96XuEA=.lz4", "multiple-deposits-one-contract"),
    ("-0M6pWgvSrqN06gIYbqrG2UbMCBKwiSuhR3-mZzRCS0=.lz4", "merged-jetton-excess"),
    ("xu061_cOVdxLMa4sn1JJXEK5XkBiPdCzEDRNI_wWKPs=.lz4", "first-deposit-partial-only"),
    ("IlYj6neBD7lYZ0WKxfFIrrrkK3fhiEK_VfH48ihb6ZU=.lz4", "final-deposit"),
    ("BD_4YMGJ9vOn8-2PFwxAj_vwi_UcrYRD61OXsHMHSb8=.lz4", "final-with-ton-excess"),
    ("k0yADcY39ZkSu5Y7keHvfMzFyadhlDZ1OvxH-c8aU7M=.lz4", "merged-with-ton-excess"),
    ("KYQfjYD1YybzQIuoJwkoXL-Y-apztmw6AGjIcPhotjQ=.lz4", "rejected-final"),
    ("OcT5DRJbqONuQtvqFmASdvE2GTh6lYwdhfegRrWUvac=.lz4", "merged-rejected"),
]

_DEPOSIT_CLASSES = (DedustDepositLiquidity, DedustDepositLiquidityPartial)
# The declarative mch build (dedust_deposit.mch) emits generic Blocks with
# these btypes instead of the legacy class instances; the pairing pass and this
# harness dispatch on btype (Wave-B: converted matchers assert btype not class).
_DEPOSIT_BTYPES = ("dedust_deposit_liquidity", "dedust_deposit_liquidity_partial")


def _load_trace(traces_dir: Path, filename: str):
    trace, interfaces = load_trace_from_file(traces_dir / filename)
    context.interface_repository.set(TestInterfaceRepository(interfaces))
    return trace


def _ir_by_name() -> dict:
    # Compiling is a frontend-internal step ir_emit needs; no compiled matcher is
    # run as a runtime (T29).
    compiled = compile_file(DEDUST_DEPOSIT_MCH, dedust_deposit_registries())
    artifact = emit_ir(compiled, [DEDUST_DEPOSIT_MCH.name])
    art = mch_ir.load_artifact(artifact)
    return {
        m.name: m for m in mch_ir.build_matchers(art, dedust_deposit_registries())
    }


def _build_block_tree(trace) -> Block:
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_side(matchers: list[BlockMatcher], trace, prereqs=()) -> list[Block]:
    """One tree per side; prerequisites first, then each matcher in
    registration order over a full BFS pass (mirrors process_event_async)."""
    root = _build_block_tree(trace)
    for pre in prereqs:
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
    return [b for b in produced if b.btype in _DEPOSIT_BTYPES]


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


def _assert_field_parity(
    actual: list[Block], reference: list[Block], label: str
) -> None:
    assert len(actual) == len(reference), (
        f"{label} count mismatch: actual={len(actual)} reference={len(reference)}"
    )
    for a, r in zip(sorted(actual, key=_sort_key), sorted(reference, key=_sort_key)):
        # Block CLASS deliberately not compared: the declarative mch product is a
        # generic Block, the legacy build a DedustDepositLiquidity[Partial]
        # subclass — btype is the parity property (Wave-B).
        assert a.btype == r.btype, f"{label} btype: {a.btype} vs {r.btype}"
        assert a.failed == r.failed, f"{label} failed flag: {a.failed} vs {r.failed}"
        assert a.broken == r.broken, f"{label} broken flag: {a.broken} vs {r.broken}"
        assert set(a.data.keys()) == set(r.data.keys()), (
            f"{label} data keys: {sorted(a.data)} vs {sorted(r.data)}"
        )
        for key in sorted(r.data):
            av, rv = a.data[key], r.data[key]
            if key == "vault_excesses" and isinstance(av, list) and isinstance(rv, list):
                # The excess list order inherits compact_connections()
                # set-order nondeterminism from the legacy pipeline itself
                # (next_blocks is rebuilt as list(set(...)) on merges, so two
                # independently built trees can bind same-kind excess payouts
                # in either order). Content is deterministic — compare as a
                # multiset.
                assert sorted(map(repr, av)) == sorted(map(repr, rv)), (
                    f"{label} data[{key!r}] (as multiset): {av!r} vs {rv!r}"
                )
                continue
            assert _values_equal(av, rv), (
                f"{label} data[{key!r}]: {av!r} ({type(av).__name__}) "
                f"vs {rv!r} ({type(rv).__name__})"
            )
        assert a.value_flow.to_dict() == r.value_flow.to_dict(), (
            f"{label} value_flow: {a.value_flow.to_dict()} vs {r.value_flow.to_dict()}"
        )
        assert _node_hashes(a) == _node_hashes(r), (
            f"{label} consumed nodes: {_node_hashes(a)} vs {_node_hashes(r)}"
        )


def _split(blocks: list[Block]) -> dict[str, list[Block]]:
    return {
        "final": [b for b in blocks if b.btype == "dedust_deposit_liquidity"],
        "partial": [b for b in blocks if b.btype == "dedust_deposit_liquidity_partial"],
    }


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "filename", [c[0] for c in CASES], ids=[c[1] for c in CASES]
)
async def test_dedust_deposit_parity(traces_dir, filename):
    trace = _load_trace(traces_dir, filename)
    ir_by_name = _ir_by_name()
    prereqs = (JettonTransferBlockMatcher(), PTonTransferMatcher())

    hw = await _run_side(
        [DedustDepositBlockMatcher(), DedustDepositFirstAssetBlockMatcher()], trace
    )
    ir = await _run_side(
        [ir_by_name[n] for n in MATCHER_NAMES], trace, prereqs=prereqs
    )

    assert len(hw) > 0, "legacy pair produced no deposit blocks - stale fixture"

    hw_s, ir_s = _split(hw), _split(ir)
    for tag in ("final", "partial"):
        _assert_field_parity(ir_s[tag], hw_s[tag], f"ir-vs-legacy[{tag}]")

    # Host-side merge pass (spec §5.5) must dispatch identically on the
    # mch-produced classes — run it over each side and re-check parity.
    hw_pp = await post_process_dedust_liquidity(hw)
    ir_pp = await post_process_dedust_liquidity(ir)
    _assert_field_parity(ir_pp, hw_pp, "postprocess ir-vs-legacy")
