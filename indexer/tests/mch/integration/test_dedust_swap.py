"""T15 — field-level parity between DedustSwapBlockMatcher and specs/dedust_swap.mch.

Same assertion depth as T13 (test_jettons_family.py): btype, deep `data`
equality, failed/broken flags, value_flow, consumed event-node set. Count
parity alone is not acceptance.

Both matchers require the prerequisite jetton_transfer/pton matchers to run
first (they pattern-match on `btype == 'jetton_transfer'`), mirroring the
production registration order in event_processing.py.

Fixtures (no dedust swap case exists in the YAML corpus; these traces were
found by scanning tests/traces for the dedust opcodes and verifying the legacy
matcher fires):
- C7bHOfSrBJPLHXMZkVnIQladzgYZHIlKOiJRvbxRRuI=  multi-hop (2 peer_swaps),
    jetton-in, jetton-out — exercises the recursive peer chain.
- c8jOrHqyuJe1hMGx4fm1BTGKXgBQ5O35rbExCkhmRG8=  multi-hop (2 peer_swaps),
    jetton-in — second recursion fixture.
- cq7UujFWNfSnpHdRKseD41yHqj4Yx3SCWsnkw8IRPoU=  single-hop, TON-in (DedustSwap
    request parent branch) — exercises the OR-parent's `op DedustSwap` arm and
    the direct payout children branch.
- 7bcODUs7IVJBjdApVMCJyUKoztjS_k4msfHyYOwt3qk=  dedust swaps whose pool data is
    absent from the interface bundle: the shared build core raises on
    get_dedust_pool -> None, and BOTH matchers must reject identically
    (spec §10.7: an exception escaping the build is a rejection).

The corpus has no jetton-in single-hop and no failed-swap (request without
notification) trace; those paths stay covered by the shared build core's use
in the legacy matcher (tests/test_actions.py corpus).
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
from indexer.events.blocks.swaps import DedustSwapBlockMatcher, JettonSwapBlock
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_file
from indexer.events.mch.builders import dedust_registries
import indexer.events.mch.specs as specs_pkg

DEDUST_MCH = Path(specs_pkg.__file__).parent / "dedust_swap.mch"

_PREREQUISITES: list[BlockMatcher] = [
    JettonTransferBlockMatcher(),
    PTonTransferMatcher(),
]


def _compile_dsl_matcher() -> BlockMatcher:
    compiled = compile_file(DEDUST_MCH, dedust_registries())
    by_name = {cm.name: cm.matcher for cm in compiled}
    return by_name["dedust_swap"]


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
    return [b for b in matches if isinstance(b, JettonSwapBlock)]


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
    assert len(hw_blocks) > 0, "legacy matcher produced no jetton_swap blocks - bad fixture"
    assert len(dsl_blocks) == len(hw_blocks), (
        f"count mismatch: dsl={len(dsl_blocks)} legacy={len(hw_blocks)}"
    )
    for d, h in zip(sorted(dsl_blocks, key=_sort_key), sorted(hw_blocks, key=_sort_key)):
        assert d.btype == h.btype == "jetton_swap"
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

        assert d.value_flow.to_dict() == h.value_flow.to_dict(), (
            f"value_flow: dsl={d.value_flow.to_dict()} legacy={h.value_flow.to_dict()}"
        )
        assert _node_hashes(d) == _node_hashes(h), (
            f"consumed event nodes: dsl={_node_hashes(d)} legacy={_node_hashes(h)}"
        )


async def _run_parity(trace, expect_peer_swaps: int) -> None:
    hw_blocks = await _run_matcher(DedustSwapBlockMatcher(), trace)
    dsl_blocks = await _run_matcher(_compile_dsl_matcher(), trace)
    _assert_field_parity(dsl_blocks, hw_blocks)
    assert len(hw_blocks[0].data["peer_swaps"]) == expect_peer_swaps, (
        "fixture no longer exercises the intended hop count"
    )


@pytest.mark.asyncio
@pytest.mark.trace_file("C7bHOfSrBJPLHXMZkVnIQladzgYZHIlKOiJRvbxRRuI=.lz4")
async def test_dedust_swap_parity_multihop(trace_data):
    """Recursive peer chain: 2-hop jetton -> jetton swap."""
    trace, _ = trace_data
    await _run_parity(trace, expect_peer_swaps=2)


@pytest.mark.asyncio
@pytest.mark.trace_file("c8jOrHqyuJe1hMGx4fm1BTGKXgBQ5O35rbExCkhmRG8=.lz4")
async def test_dedust_swap_parity_multihop_2(trace_data):
    """Second multi-hop fixture (different pools/assets)."""
    trace, _ = trace_data
    await _run_parity(trace, expect_peer_swaps=2)


@pytest.mark.asyncio
@pytest.mark.trace_file("cq7UujFWNfSnpHdRKseD41yHqj4Yx3SCWsnkw8IRPoU=.lz4")
async def test_dedust_swap_parity_single_hop_ton_in(trace_data):
    """Single-hop TON-in swap: `op DedustSwap` parent arm + direct payout chain
    (peer_swaps stays [] for single-hop)."""
    trace, _ = trace_data
    await _run_parity(trace, expect_peer_swaps=0)


@pytest.mark.asyncio
@pytest.mark.trace_file("7bcODUs7IVJBjdApVMCJyUKoztjS_k4msfHyYOwt3qk=.lz4")
async def test_dedust_swap_both_reject_on_build_error(trace_data):
    """Pool data missing from the interface bundle: the shared build core
    raises, and per spec §10.7 both the legacy matcher and the mch matcher must
    reject (produce nothing) rather than abort."""
    trace, _ = trace_data
    hw_blocks = await _run_matcher(DedustSwapBlockMatcher(), trace)
    dsl_blocks = await _run_matcher(_compile_dsl_matcher(), trace)
    assert hw_blocks == []
    assert dsl_blocks == []
