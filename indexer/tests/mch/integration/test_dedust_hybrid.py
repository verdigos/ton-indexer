"""Fn-escape spike — field-level parity between DedustSwapBlockMatcher and the
HYBRID declarative build (specs_decl/dedust_swap_hybrid.mch: build statements
+ `out {...}` + registered host fns, no `build` directive).

Same fixtures and acceptance bar as test_dedust_swap.py: btype, deep `data`
equality, failed/broken flags, value_flow, consumed event-node set. The hybrid
path produces a generic Block (declarative builds don't instantiate host block
classes), so blocks are selected by btype, not isinstance.
"""
from __future__ import annotations

from pathlib import Path

import pytest

from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.jettons import (
    JettonTransferBlockMatcher,
    PTonTransferMatcher,
)
from indexer.events.blocks.swaps import DedustSwapBlockMatcher
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
import indexer.events.mch as mch
from indexer.events.mch import compile_file
from indexer.events.mch.builders import dedust_hybrid_registries

HYBRID_MCH = Path(mch.__file__).parent / "specs_decl" / "dedust_swap_hybrid.mch"

_PREREQUISITES: list[BlockMatcher] = [
    JettonTransferBlockMatcher(),
    PTonTransferMatcher(),
]


def _compile_hybrid_matcher() -> BlockMatcher:
    compiled = compile_file(HYBRID_MCH, dedust_hybrid_registries())
    by_name = {cm.name: cm.matcher for cm in compiled}
    return by_name["dedust_swap_hybrid"]


def _build_block_tree(trace) -> Block:
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matcher(matcher: BlockMatcher, trace) -> list:
    root = _build_block_tree(trace)
    for pre in _PREREQUISITES:
        for b in root.bfs_iter():
            if b.parent is None:
                await pre.try_build(b)
    matches: list = []
    for b in root.bfs_iter():
        if b.btype == "root":
            continue
        if b.parent is not None:
            continue
        result = await matcher.try_build(b)
        if result is not None:
            matches.extend(result)
    return [b for b in matches if b.btype == "jetton_swap"]


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


def _assert_field_parity(hybrid_blocks: list[Block], hw_blocks: list[Block]) -> None:
    assert len(hw_blocks) > 0, "legacy matcher produced no jetton_swap blocks - bad fixture"
    assert len(hybrid_blocks) == len(hw_blocks), (
        f"count mismatch: hybrid={len(hybrid_blocks)} legacy={len(hw_blocks)}"
    )
    for d, h in zip(sorted(hybrid_blocks, key=_sort_key), sorted(hw_blocks, key=_sort_key)):
        assert d.btype == h.btype == "jetton_swap"
        assert d.failed == h.failed, f"failed flag: hybrid={d.failed} legacy={h.failed}"
        assert d.broken == h.broken, f"broken flag: hybrid={d.broken} legacy={h.broken}"

        assert set(d.data.keys()) == set(h.data.keys()), (
            f"data keys: hybrid={sorted(d.data)} legacy={sorted(h.data)}"
        )
        for key in sorted(h.data):
            dv, hv = d.data[key], h.data[key]
            assert _values_equal(dv, hv), (
                f"data[{key!r}]: hybrid={dv!r} ({type(dv).__name__}) "
                f"legacy={hv!r} ({type(hv).__name__})"
            )

        assert d.value_flow.to_dict() == h.value_flow.to_dict(), (
            f"value_flow: hybrid={d.value_flow.to_dict()} legacy={h.value_flow.to_dict()}"
        )
        assert _node_hashes(d) == _node_hashes(h), (
            f"consumed event nodes: hybrid={_node_hashes(d)} legacy={_node_hashes(h)}"
        )


async def _run_parity(trace, expect_peer_swaps: int) -> None:
    hw_blocks = await _run_matcher(DedustSwapBlockMatcher(), trace)
    hybrid_blocks = await _run_matcher(_compile_hybrid_matcher(), trace)
    _assert_field_parity(hybrid_blocks, hw_blocks)
    assert len(hw_blocks[0].data["peer_swaps"]) == expect_peer_swaps, (
        "fixture no longer exercises the intended hop count"
    )


@pytest.mark.asyncio
@pytest.mark.trace_file("C7bHOfSrBJPLHXMZkVnIQladzgYZHIlKOiJRvbxRRuI=.lz4")
async def test_dedust_hybrid_parity_multihop(trace_data):
    """Recursive peer chain: 2-hop jetton -> jetton swap."""
    trace, _ = trace_data
    await _run_parity(trace, expect_peer_swaps=2)


@pytest.mark.asyncio
@pytest.mark.trace_file("c8jOrHqyuJe1hMGx4fm1BTGKXgBQ5O35rbExCkhmRG8=.lz4")
async def test_dedust_hybrid_parity_multihop_2(trace_data):
    """Second multi-hop fixture (different pools/assets)."""
    trace, _ = trace_data
    await _run_parity(trace, expect_peer_swaps=2)


@pytest.mark.asyncio
@pytest.mark.trace_file("cq7UujFWNfSnpHdRKseD41yHqj4Yx3SCWsnkw8IRPoU=.lz4")
async def test_dedust_hybrid_parity_single_hop_ton_in(trace_data):
    """Single-hop TON-in swap: the declarative `op DedustSwap` sender/asset arm
    + direct payout children branch (peer_swaps stays [] for single-hop)."""
    trace, _ = trace_data
    await _run_parity(trace, expect_peer_swaps=0)


@pytest.mark.asyncio
@pytest.mark.trace_file("7bcODUs7IVJBjdApVMCJyUKoztjS_k4msfHyYOwt3qk=.lz4")
async def test_dedust_hybrid_both_reject_on_build_error(trace_data):
    """Pool data missing from the interface bundle: the host fn raises inside
    the pool-hop walk -> EvalError -> rejection (spec §10.7); the legacy
    matcher rejects via the try_build exception guard. Both produce nothing."""
    trace, _ = trace_data
    hw_blocks = await _run_matcher(DedustSwapBlockMatcher(), trace)
    hybrid_blocks = await _run_matcher(_compile_hybrid_matcher(), trace)
    assert hw_blocks == []
    assert hybrid_blocks == []
