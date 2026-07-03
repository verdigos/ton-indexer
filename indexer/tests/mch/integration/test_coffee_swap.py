"""T17 — field-level parity between CoffeeSwapBlockMatcher and specs/coffee_swap.mch.

Same assertion depth as T13/T15: btype, deep `data` equality, failed/broken
flags, value_flow, consumed event-node set. Count parity alone is not
acceptance.

Both matchers require the prerequisite jetton_transfer/pton matchers to run
first (in-transfer and jetton payouts pattern-match on `btype ==
'jetton_transfer'`), mirroring the production registration order in
event_processing.py.

Fixtures (coffee.yaml corpus + coffee legs found in multi-dex traces by
scanning tests/traces and verifying the legacy matcher fires):
- dp1xdxzIC7tCyZfy80VTW5be7SimpOOTnuf3_8BZjb0=  multi-hop (2 peer_swaps),
    jetton-in, jetton-out — exercises the recursive swap chain (step = swap ->
    success-event two-node chain).
- p1-SZ1wV7yEe2qf1SQujTW5OMifEPuuX-jPPrPNiKxw=  single-hop, jetton-in, TON-out
    — the direct payout branch of the anchor's children-set alternative.
- cq7UujFWNfSnpHdRKseD41yHqj4Yx3SCWsnkw8IRPoU=  single-hop, TON-in via
    `op CoffeeSwapNative` (the second arm of the same-name @in_transfer parent
    XOR, T02), jetton-out; coffee leg inside a multi-dex trace.
- C7bHOfSrBJPLHXMZkVnIQladzgYZHIlKOiJRvbxRRuI=  single-hop coffee leg inside
    the "Complicated Swap (Dedust, Coffee + Stonfi)" trace, TON-out.

The corpus has no failed coffee swap (a swap_internal without its success
event); that path stays covered by the shared build core's use in the legacy
matcher (tests/test_actions.py corpus).
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
from indexer.events.blocks.swaps import CoffeeSwapBlockMatcher, JettonSwapBlock
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_file
from indexer.events.mch.builders import coffee_registries
import indexer.events.mch.specs as specs_pkg

COFFEE_MCH = Path(specs_pkg.__file__).parent / "coffee_swap.mch"

_PREREQUISITES: list[BlockMatcher] = [
    JettonTransferBlockMatcher(),
    PTonTransferMatcher(),
]


def _compile_dsl_matcher() -> BlockMatcher:
    compiled = compile_file(COFFEE_MCH, coffee_registries())
    by_name = {cm.name: cm.matcher for cm in compiled}
    return by_name["coffee_swap"]


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
    return [
        b for b in matches
        if isinstance(b, JettonSwapBlock) and b.data.get("dex") == "coffee"
    ]


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
    assert len(hw_blocks) > 0, "legacy matcher produced no coffee jetton_swap blocks - bad fixture"
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
    hw_blocks = await _run_matcher(CoffeeSwapBlockMatcher(), trace)
    dsl_blocks = await _run_matcher(_compile_dsl_matcher(), trace)
    _assert_field_parity(dsl_blocks, hw_blocks)
    assert len(hw_blocks[0].data["peer_swaps"]) == expect_peer_swaps, (
        "fixture no longer exercises the intended hop count"
    )


@pytest.mark.asyncio
@pytest.mark.trace_file("dp1xdxzIC7tCyZfy80VTW5be7SimpOOTnuf3_8BZjb0=.lz4")
async def test_coffee_swap_parity_multihop(trace_data):
    """Recursive swap chain: 2-hop USDT -> TON -> CES swap (swap+event step
    consumed per unrolling)."""
    trace, _ = trace_data
    await _run_parity(trace, expect_peer_swaps=2)


@pytest.mark.asyncio
@pytest.mark.trace_file("p1-SZ1wV7yEe2qf1SQujTW5OMifEPuuX-jPPrPNiKxw=.lz4")
async def test_coffee_swap_parity_single_hop_ton_out(trace_data):
    """Single-hop jetton -> TON swap: direct payout branch (no recursion),
    TON-payout terminal."""
    trace, _ = trace_data
    await _run_parity(trace, expect_peer_swaps=0)


@pytest.mark.asyncio
@pytest.mark.trace_file("cq7UujFWNfSnpHdRKseD41yHqj4Yx3SCWsnkw8IRPoU=.lz4")
async def test_coffee_swap_parity_native_ton_in(trace_data):
    """Single-hop TON -> jetton swap: `op CoffeeSwapNative` parent arm (the
    other branch of the same-name @in_transfer XOR), jetton-out terminal."""
    trace, _ = trace_data
    await _run_parity(trace, expect_peer_swaps=0)


@pytest.mark.asyncio
@pytest.mark.trace_file("C7bHOfSrBJPLHXMZkVnIQladzgYZHIlKOiJRvbxRRuI=.lz4")
async def test_coffee_swap_parity_multidex_leg(trace_data):
    """Coffee leg embedded in a multi-dex trace (Dedust, Coffee + Stonfi):
    the anchor sits mid-trace, in-transfer is a jetton_transfer produced by
    another dex's payout."""
    trace, _ = trace_data
    await _run_parity(trace, expect_peer_swaps=0)
