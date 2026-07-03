"""T16 — field-level parity between StonfiV2SwapBlockMatcher and
specs/stonfi_v2_swap.mch.

Same assertion depth as T13/T15: btype, deep `data` equality, failed/broken
flags, value_flow, consumed event-node set. Count parity alone is not
acceptance. The `pton_self_transfer` predicate is the real payload-validating
implementation (shared with the legacy matcher), not a stub.

Fixtures (YAML corpus grep for `dex: stonfi_v2` + legacy-matcher scan over all
147 traces in tests/traces):
- vdNpUDA43aIw6QlbcKMgi7q4S0MnJAVTfXVRqbAt5Tw=  single-hop jetton -> jetton
    (YAML "Stonfi v2 Jetton -> Jetton").
- CD5IIyWGj3nbuAyIEgmeMGJ28UdC4lMHqRRYcxdLraE=  second single-hop
    jetton -> jetton.
- 00BTqIs6w2qgKXyjfUIwS1WMPbYzWvOn0P16eku_O-4=  2-hop multirouter
    jetton -> jetton (YAML "Stonfi v2 Jetton -> Jetton multirouter") —
    exercises the recursion via the plain jetton_transfer hop branch.
- TAi-sZDDdv1zFm6RQMu0UmQdYq9_MuNvFBSfSWiqgL4=  2-hop jetton -> TON -> jetton
    (YAML "Stonfi Jetton -> Ton -> Jetton") — exercises the intermediate-pton
    branch: `where no_internal_transfer` + `pred pton_self_transfer` for real.
- C7bHOfSrBJPLHXMZkVnIQladzgYZHIlKOiJRvbxRRuI=  single-hop TON-in inside a
    multi-dex trace — exercises the `op JettonNotify <- op PTonTransfer`
    in-transfer arm.
- cq7UujFWNfSnpHdRKseD41yHqj4Yx3SCWsnkw8IRPoU=  two independent stonfi_v2
    swaps (one TON-in, one jetton-in) in one trace.

Corpus gaps (no recording per task rules): no failed swap (pay_to exit code
!= 0xc64370e5 — destination_asset fallback paths unexercised), no terminal
TON/pton out-transfer (additional_blocks_to_include path), no broken flag
(wallet lookup miss mid-sequence).
"""
from __future__ import annotations

from pathlib import Path

import pytest

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.jettons import (
    JettonTransferBlockMatcher,
    PTonTransferMatcher,
)
from indexer.events.blocks.messages import PTonTransfer
from indexer.events.blocks.swaps import JettonSwapBlock, StonfiV2SwapBlockMatcher
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_file
from indexer.events.mch.builders import stonfi_v2_registries
import indexer.events.mch.specs as specs_pkg

STONFI_V2_MCH = Path(specs_pkg.__file__).parent / "stonfi_v2_swap.mch"

_PREREQUISITES: list[BlockMatcher] = [
    JettonTransferBlockMatcher(),
    PTonTransferMatcher(),
]


def _compile_dsl_matcher() -> BlockMatcher:
    compiled = compile_file(STONFI_V2_MCH, stonfi_v2_registries())
    by_name = {cm.name: cm.matcher for cm in compiled}
    return by_name["stonfi_v2_swap"]


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
    return [b for b in matches
            if isinstance(b, JettonSwapBlock) and b.data.get("dex") == "stonfi_v2"]


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
    assert len(hw_blocks) > 0, "legacy matcher produced no stonfi_v2 blocks - bad fixture"
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


async def _run_parity(trace) -> tuple[list, list]:
    hw_blocks = await _run_matcher(StonfiV2SwapBlockMatcher(), trace)
    dsl_blocks = await _run_matcher(_compile_dsl_matcher(), trace)
    _assert_field_parity(dsl_blocks, hw_blocks)
    return dsl_blocks, hw_blocks


@pytest.mark.asyncio
@pytest.mark.trace_file("vdNpUDA43aIw6QlbcKMgi7q4S0MnJAVTfXVRqbAt5Tw=.lz4")
async def test_stonfi_v2_parity_single_hop(trace_data):
    """Single-hop jetton -> jetton (peer_swaps stays [])."""
    trace, _ = trace_data
    _, hw = await _run_parity(trace)
    assert hw[0].data["peer_swaps"] == []


@pytest.mark.asyncio
@pytest.mark.trace_file("CD5IIyWGj3nbuAyIEgmeMGJ28UdC4lMHqRRYcxdLraE=.lz4")
async def test_stonfi_v2_parity_single_hop_2(trace_data):
    """Second single-hop jetton -> jetton fixture."""
    trace, _ = trace_data
    await _run_parity(trace)


@pytest.mark.asyncio
@pytest.mark.trace_file("00BTqIs6w2qgKXyjfUIwS1WMPbYzWvOn0P16eku_O-4=.lz4")
async def test_stonfi_v2_parity_multihop(trace_data):
    """2-hop multirouter swap: recursion via the plain jetton_transfer branch
    (roadmap acceptance: multi-hop fixture)."""
    trace, _ = trace_data
    _, hw = await _run_parity(trace)
    assert len(hw[0].data["peer_swaps"]) == 2, (
        "fixture no longer exercises the intended hop count"
    )


@pytest.mark.asyncio
@pytest.mark.trace_file("TAi-sZDDdv1zFm6RQMu0UmQdYq9_MuNvFBSfSWiqgL4=.lz4")
async def test_stonfi_v2_parity_multihop_intermediate_pton(trace_data):
    """2-hop jetton -> TON -> jetton: the hop between routers goes through a
    pton transfer, so the real `pton_self_transfer` predicate must accept the
    intermediate transfer call for the DSL pattern to reach hop 2."""
    trace, _ = trace_data
    dsl, hw = await _run_parity(trace)
    assert len(hw[0].data["peer_swaps"]) == 2
    assert hw[0].data["peer_swaps"][0]["out"]["asset"].is_ton, (
        "fixture no longer has a TON mid-hop"
    )
    # The validated pton transfer call must be consumed by the DSL pattern
    # (legacy consumes it through its GenericMatcher chain).
    assert any(isinstance(c, CallContractBlock) and c.opcode == PTonTransfer.opcode
               for c in dsl[0].children_blocks), (
        "intermediate pton transfer call not absorbed by the DSL match"
    )


@pytest.mark.asyncio
@pytest.mark.trace_file("C7bHOfSrBJPLHXMZkVnIQladzgYZHIlKOiJRvbxRRuI=.lz4")
async def test_stonfi_v2_parity_ton_in(trace_data):
    """TON-in swap: `op JettonNotify <- op PTonTransfer` in-transfer arm,
    inside a multi-dex trace (the dedust legs stay unconsumed)."""
    trace, _ = trace_data
    _, hw = await _run_parity(trace)
    assert hw[0].data["dex_incoming_transfer"]["asset"].is_ton, (
        "fixture no longer has a TON-in leg"
    )


@pytest.mark.asyncio
@pytest.mark.trace_file("cq7UujFWNfSnpHdRKseD41yHqj4Yx3SCWsnkw8IRPoU=.lz4")
async def test_stonfi_v2_parity_two_swaps_in_one_trace(trace_data):
    """Two independent stonfi_v2 swaps (one TON-in, one jetton-in) in a single
    trace — both matched, pairwise field-equal."""
    trace, _ = trace_data
    _, hw = await _run_parity(trace)
    assert len(hw) == 2
    in_kinds = sorted(b.data["dex_incoming_transfer"]["asset"].is_ton for b in hw)
    assert in_kinds == [False, True], "fixture no longer covers both in-arms"
