"""R11 (= T28) — three-way field-level parity for the tonco swap port:
legacy ToncoSwapBlockMatcher == compiled specs/tonco_swap.mch == v2 IR engine.

First port to use the CYCLIC recursion strategy (`rule tonco_hop cyclic`,
spec §10.3): legacy tonco is a cyclic child-matcher graph where hop
continuation is mandatory and the final branches share the
`swap -> payout -> jetton_transfer` prefix with the hop branches — the
frontier strategy's exit-first order would terminate multi-hop chains at
hop 2 (the T28 premature-exit problem).

Assertion depth per the M2R remake gate: btype, deep `data` equality,
failed/broken flags, value_flow, consumed event-node set, and block topology
(children_blocks btype multiset — the action hierarchy derives from it).

Fixtures (tests/test_cases/tonco.yaml swap cases):
- g1JLQayzzERU9tojKPorN9Asd-MCF_0omnRkwOd_Olo=  single-hop USDT -> TON
    (jetton in-arm, pton out through the final-jetton branch).
- CjtIXjqazbSFrtPqqMMFBgMG6-huwp5CfNtXpQAf8xY=  2-hop USDT -> TON -> STORM —
    the premature-exit killer: hop 1's payout child must bind the
    hop-continuation branch, not the same-prefix final branch.
- 02TsVcC_MOggAZvHHBITbHCvzAt_hCq5WVAiKcZFMno=  failed TON -> USDT
    (exit_code != 0/200 refund; destination_asset extracted from the swap
    payload's target wallets — the failed-swap fallback path).
- 03dhjTFKdEV_03M0b0aDfZvdlWxVoUutqXaXRyYG9nw=  TON -> KUKU
    (`op JettonNotify <- op PTonTransfer` in-transfer arm).
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
from indexer.events.blocks.swaps import JettonSwapBlock, ToncoSwapBlockMatcher
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_file
from indexer.events.mch.builders.tonco_swap import tonco_swap_registries
from indexer.events.mch.ir_emit import emit_ir
from indexer.events import mch_ir
import indexer.events.mch.specs as specs_pkg

TONCO_MCH = Path(specs_pkg.__file__).parent / "tonco_swap.mch"

_PREREQUISITES = (JettonTransferBlockMatcher, PTonTransferMatcher)


def _ir_matcher():
    # Compiling is a frontend-internal step ir_emit needs; no compiled matcher is
    # run as a runtime (T29).
    compiled = compile_file(TONCO_MCH, tonco_swap_registries())
    artifact = emit_ir(compiled, [TONCO_MCH.name])
    art = mch_ir.load_artifact(artifact)
    return {m.name: m for m in mch_ir.build_matchers(art, tonco_swap_registries())}["tonco_swap"]


def _build_block_tree(trace) -> Block:
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matcher(matcher, trace) -> list:
    root = _build_block_tree(trace)
    for pre_cls in _PREREQUISITES:
        pre = pre_cls()
        for b in root.bfs_iter():
            if b.parent is None:
                await pre.try_build(b)
    matches: list = []
    for b in root.bfs_iter():
        if b.btype == "root" or b.parent is not None:
            continue
        result = await matcher.try_build(b)
        if result is not None:
            matches.extend(result)
    return [b for b in matches
            if isinstance(b, JettonSwapBlock) and b.data.get("dex") == "tonco"]


def _values_equal(a, b) -> bool:
    if a is None and b is None:
        return True
    if type(a) is not type(b):
        return False
    if isinstance(a, list):
        # Order-bearing collections can flip between independently built trees
        # (legacy compact_connections rebuilds next_blocks via set, T21);
        # compare as multisets like the parity sweep does.
        if len(a) != len(b):
            return False
        if a == b:
            return True
        return sorted(map(repr, a)) == sorted(map(repr, b))
    return bool(a == b)


def _node_hashes(block: Block) -> list[str]:
    return sorted(n.message.tx_hash for n in block.event_nodes)


def _topology(block: Block) -> list[str]:
    """Produced block's immediate tree shape: children btypes + next btypes.
    The remake gate demands block-topology parity because serialize_blocks
    derives the action hierarchy from it."""
    return [
        sorted(c.btype for c in block.children_blocks),
        sorted(c.btype for c in block.next_blocks),
    ]


def _sort_key(block: Block):
    return (block.min_lt, tuple(_node_hashes(block)))


def _assert_parity(label: str, got: list[Block], ref: list[Block]) -> None:
    assert len(got) == len(ref), (
        f"{label}: count mismatch got={len(got)} legacy={len(ref)}"
    )
    for g, r in zip(sorted(got, key=_sort_key), sorted(ref, key=_sort_key)):
        assert g.btype == r.btype == "jetton_swap"
        assert g.failed == r.failed, f"{label} failed flag: {g.failed} != {r.failed}"
        assert g.broken == r.broken, f"{label} broken flag: {g.broken} != {r.broken}"
        assert set(g.data.keys()) == set(r.data.keys()), (
            f"{label} data keys: {sorted(g.data)} != {sorted(r.data)}"
        )
        for key in sorted(r.data):
            gv, rv = g.data[key], r.data[key]
            assert _values_equal(gv, rv), (
                f"{label} data[{key!r}]: got={gv!r} ({type(gv).__name__}) "
                f"legacy={rv!r} ({type(rv).__name__})"
            )
        assert g.value_flow.to_dict() == r.value_flow.to_dict(), (
            f"{label} value_flow: {g.value_flow.to_dict()} != {r.value_flow.to_dict()}"
        )
        assert _node_hashes(g) == _node_hashes(r), (
            f"{label} consumed event nodes: {_node_hashes(g)} != {_node_hashes(r)}"
        )
        assert _topology(g) == _topology(r), (
            f"{label} topology: {_topology(g)} != {_topology(r)}"
        )


async def _run_three_way(trace) -> list[Block]:
    """Legacy is the reference; compiled and IR must both match it (and hence
    each other). Returns the legacy blocks for fixture-shape assertions."""
    legacy = await _run_matcher(ToncoSwapBlockMatcher(), trace)
    assert legacy, "legacy matcher produced no tonco blocks - bad fixture"
    ir = await _run_matcher(_ir_matcher(), trace)
    _assert_parity("ir", ir, legacy)
    return legacy


@pytest.mark.asyncio
@pytest.mark.trace_file("g1JLQayzzERU9tojKPorN9Asd-MCF_0omnRkwOd_Olo=.lz4")
async def test_tonco_parity_single_hop(trace_data):
    """Single-hop USDT -> TON (peer_swaps stays [])."""
    trace, _ = trace_data
    legacy = await _run_three_way(trace)
    assert legacy[0].data["peer_swaps"] == []


@pytest.mark.asyncio
@pytest.mark.trace_file("CjtIXjqazbSFrtPqqMMFBgMG6-huwp5CfNtXpQAf8xY=.lz4")
async def test_tonco_parity_multihop(trace_data):
    """2-hop USDT -> TON -> STORM — the T28 premature-exit killer: the
    frontier strategy would bind hop 1's payout child to the same-prefix
    final branch and stop; cyclic descent must reach hop 2."""
    trace, _ = trace_data
    legacy = await _run_three_way(trace)
    assert len(legacy[0].data["peer_swaps"]) == 2, (
        "fixture no longer exercises the intended hop count"
    )


@pytest.mark.asyncio
@pytest.mark.trace_file("02TsVcC_MOggAZvHHBITbHCvzAt_hCq5WVAiKcZFMno=.lz4")
async def test_tonco_parity_failed_swap(trace_data):
    """Failed TON -> USDT: pay_to exit code != 0/200; destination_asset comes
    from the swap payload's recursive target-wallet extraction, not from the
    (refund) out transfer."""
    trace, _ = trace_data
    legacy = await _run_three_way(trace)
    assert legacy[0].failed, "fixture no longer covers the failed-swap path"


@pytest.mark.asyncio
@pytest.mark.trace_file("03dhjTFKdEV_03M0b0aDfZvdlWxVoUutqXaXRyYG9nw=.lz4")
async def test_tonco_parity_ton_in(trace_data):
    """TON -> KUKU: the `op JettonNotify <- op PTonTransfer` in-transfer arm."""
    trace, _ = trace_data
    legacy = await _run_three_way(trace)
    assert legacy[0].data["dex_incoming_transfer"]["asset"].is_ton, (
        "fixture no longer has a TON-in leg"
    )
