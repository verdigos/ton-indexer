"""R12 — three-way field-level parity for the stonfi v1 swap remake:
legacy StonfiSwapBlockMatcher == compiled specs/stonfi_v1_swap.mch == v2 IR.

The remade matcher kills the legacy build-side consumption surgery: legacy
matches every payment request (any exit code) and bfs-removes the ok_ref
referral subtree from `other_blocks` during build; the mch pattern gates the
payment-request node on the sender-related exit codes so the referral subtree
is never consumed, while the builder reads the referral amount/address from
the unconsumed sibling through the tree. Consumed-set parity (event-node
hashes) on the referral fixture is therefore THE load-bearing assertion here.

Assertion depth per the M2R remake gate: btype, deep `data` equality,
failed/broken flags, value_flow, consumed event-node set, block topology.

Fixtures — every stonfi v1 case in tests/test_cases/stonfi-swaps.yaml:
- utNOlIUjzWv7MRoSUfkjHkBaIg40RkYMLOy9ljAaykQ=  jetton -> TON with referral
    payout (ok_ref payment request; the surgery case — referral data set,
    referral subtree NOT consumed, classifies as its own jetton_transfer).
- 01SkGWslZ8iEn3bXyb0ga5dWB5gRzw8zWAuwfUI4ZMA=  jetton -> TON, receiver != sender.
- 4PNVGQkZJUWAiiPq3rIekA9O5-HLaXmYmP30iixAefA=  jetton -> TON, receiver == sender.
- E_ktemwUPyP4hfuuBbuf3DUnN33611NaKQhG3odLZao=  jetton -> jetton.
- jwYkEIdwbHJAGiZD6JYHs_TWBBdJQBWK0hIfwDL1wqU=  TON -> jetton FAILED
    (no_liq/reserve_err exit code refund; failed flag set).
- T4ZexTBP42cObyNEUyj8zpd2kDXElijp03HzIIVMqvQ=  TON -> jetton, receiver != sender.
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
from indexer.events.blocks.messages import StonfiPaymentRequest
from indexer.events.blocks.swaps import (
    JettonSwapBlock,
    StonfiSwapBlockMatcher,
    stonfi_swap_ok_ref_exit_code,
)
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_file
from indexer.events.mch.builders import stonfi_v1_registries
from indexer.events.mch.ir_emit import emit_ir
from indexer.events import mch_ir
import indexer.events.mch.specs as specs_pkg

STONFI_V1_MCH = Path(specs_pkg.__file__).parent / "stonfi_v1_swap.mch"

_PREREQUISITES = (JettonTransferBlockMatcher, PTonTransferMatcher)


def _ir_matcher():
    # Compiling is a frontend-internal step ir_emit needs; no compiled matcher is
    # run as a runtime (T29).
    compiled = compile_file(STONFI_V1_MCH, stonfi_v1_registries())
    artifact = emit_ir(compiled, [STONFI_V1_MCH.name])
    art = mch_ir.load_artifact(artifact)
    return {m.name: m
            for m in mch_ir.build_matchers(art, stonfi_v1_registries())}["stonfi_v1_swap"]


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
    # Wave-B: the hybrid build produces a plain Block with btype 'jetton_swap'
    # (not a JettonSwapBlock instance); filter on btype, not class identity.
    return [b for b in matches
            if b.btype == "jetton_swap" and b.data.get("dex") == "stonfi"]


def _values_equal(a, b) -> bool:
    if a is None and b is None:
        return True
    if type(a) is not type(b):
        return False
    if isinstance(a, list):
        if len(a) != len(b):
            return False
        if a == b:
            return True
        return sorted(map(repr, a)) == sorted(map(repr, b))
    return bool(a == b)


def _node_hashes(block: Block) -> list[str]:
    return sorted(n.message.tx_hash for n in block.event_nodes)


def _topology(block: Block) -> list[list[str]]:
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
    legacy = await _run_matcher(StonfiSwapBlockMatcher(), trace)
    assert legacy, "legacy matcher produced no stonfi blocks - bad fixture"
    ir = await _run_matcher(_ir_matcher(), trace)
    _assert_parity("ir", ir, legacy)
    return legacy


@pytest.mark.asyncio
@pytest.mark.trace_file("utNOlIUjzWv7MRoSUfkjHkBaIg40RkYMLOy9ljAaykQ=.lz4")
async def test_stonfi_v1_parity_referral_payout(trace_data):
    """The surgery case: ok_ref referral payment. Referral data must be read
    from the referral payment request, but its subtree must NOT be part of the
    produced block (legacy bfs-removes it; the remake never consumes it)."""
    trace, _ = trace_data
    legacy = await _run_three_way(trace)
    assert legacy[0].data["referral_address"] is not None, (
        "fixture no longer carries a referral payout"
    )
    # The ok_ref payment-request's event nodes stay OUT of the consumed set.
    consumed = set(_node_hashes(legacy[0]))
    swap_call = next(b for b in legacy[0].children_blocks
                     if isinstance(b, CallContractBlock)
                     and b.opcode == 0x25938561)
    ref_requests = [
        x for x in swap_call.next_blocks
        if isinstance(x, CallContractBlock)
        and x.opcode == StonfiPaymentRequest.opcode
        and StonfiPaymentRequest(x.get_body()).exit_code == stonfi_swap_ok_ref_exit_code
    ]
    assert ref_requests, "fixture no longer has an ok_ref payment request"
    for rr in ref_requests:
        assert not (set(_node_hashes(rr)) & consumed), (
            "referral payment request leaked into the consumed set"
        )


@pytest.mark.asyncio
@pytest.mark.trace_file("01SkGWslZ8iEn3bXyb0ga5dWB5gRzw8zWAuwfUI4ZMA=.lz4")
async def test_stonfi_v1_parity_jetton_to_ton_receiver_differs(trace_data):
    trace, _ = trace_data
    legacy = await _run_three_way(trace)
    d = legacy[0].data
    assert d["sender"] != d["receiver"], "fixture no longer has receiver != sender"


@pytest.mark.asyncio
@pytest.mark.trace_file("4PNVGQkZJUWAiiPq3rIekA9O5-HLaXmYmP30iixAefA=.lz4")
async def test_stonfi_v1_parity_jetton_to_ton_receiver_same(trace_data):
    trace, _ = trace_data
    legacy = await _run_three_way(trace)
    d = legacy[0].data
    assert d["sender"] == d["receiver"], "fixture no longer has receiver == sender"


@pytest.mark.asyncio
@pytest.mark.trace_file("E_ktemwUPyP4hfuuBbuf3DUnN33611NaKQhG3odLZao=.lz4")
async def test_stonfi_v1_parity_jetton_to_jetton(trace_data):
    trace, _ = trace_data
    legacy = await _run_three_way(trace)
    assert not legacy[0].data["dex_incoming_transfer"]["asset"].is_ton
    assert not legacy[0].data["dex_outgoing_transfer"]["asset"].is_ton


@pytest.mark.asyncio
@pytest.mark.trace_file("jwYkEIdwbHJAGiZD6JYHs_TWBBdJQBWK0hIfwDL1wqU=.lz4")
async def test_stonfi_v1_parity_failed_swap(trace_data):
    """TON -> jetton with a non-ok sender-related exit code: success=False,
    failed flag set, refund transfer is the out leg."""
    trace, _ = trace_data
    legacy = await _run_three_way(trace)
    assert legacy[0].failed, "fixture no longer covers the failed-swap path"


@pytest.mark.asyncio
@pytest.mark.trace_file("T4ZexTBP42cObyNEUyj8zpd2kDXElijp03HzIIVMqvQ=.lz4")
async def test_stonfi_v1_parity_ton_in_receiver_differs(trace_data):
    trace, _ = trace_data
    legacy = await _run_three_way(trace)
    # TON-in rides through pTON in v1: the in leg has no source jetton wallet
    # (asset itself is the pTON jetton master, not is_ton).
    d = legacy[0].data
    assert d["dex_incoming_transfer"]["source_jetton_wallet"] is None, (
        "fixture no longer has a TON(pTON)-in leg"
    )
    assert d["sender"] != d["receiver"], "fixture no longer has receiver != sender"
