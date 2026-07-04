"""R05 — field-level parity between the legacy NominatorPoolWithdrawMatcher and
specs/nominator.mch, plus the IR round trip (compile -> emit_ir -> load_artifact
-> build_matchers).

This is a REMAKE, not a port (see specs/nominator.mch header for why): legacy
anchors on the NominatorPoolProcessWithdrawRequests call and fans out one
NominatorPoolWithdrawRequestBlock per ton_transfer child, leaving the call
itself unconsumed. The mch pattern re-anchors on each payout ton_transfer
directly (checking its parent call via a host predicate, never a captured
node), so the engine's normal per-block loop reproduces the fan-out without
needing multi-block builder returns (which the compiled build path does not
support - see the spec header).

No nominator YAML fixture exists in the corpus, and the flow needs no
text-comment trigger (unlike NominatorPoolDepositMatcher / withdraw-request,
which key off ton_transfer comments 'd'/'w' - out of scope here): the target
matcher is a plain opcode-anchored call -> ton_transfer fan-out, so synthetic-
but-faithful trees (real Transaction/Message -> to_tree -> init_block) fully
exercise it, same convention as test_dns.py / test_evaa_withdraw.py.
"""
from __future__ import annotations

from pathlib import Path

import pytest
from pytoniq_core import Address, begin_cell

from indexer.core.database import Message, MessageContent, Transaction
from indexer.events import context
from indexer.events.blocks.core import Block
from indexer.events.blocks.staking import NominatorPoolWithdrawMatcher, NominatorPoolWithdrawRequestData
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_file
from indexer.events.mch.builders.nominator import nominator_registries
from indexer.events.mch.ir_emit import emit_ir, render_json
import indexer.events.mch.specs as specs_pkg
from tests.utils.repository import TestInterfaceRepository

# --- IR engine gate (same convention as test_evaa_withdraw / test_dns) ------
try:
    from indexer.events import mch_ir as _mch_ir

    _HAVE_IR = hasattr(_mch_ir, "load_artifact") and hasattr(_mch_ir, "build_matchers")
except Exception:  # pragma: no cover - import error path
    _mch_ir = None
    _HAVE_IR = False

_SKIP_REASON = (
    "indexer.events.mch_ir not importable; the legacy-vs-compiled parity and "
    "the IR emit above still ran"
)

NOMINATOR_MCH = Path(specs_pkg.__file__).parent / "nominator.mch"

WITHDRAW_OPCODE = 0x00000002  # NominatorPoolProcessWithdrawRequests

POOL_ADDR = Address("0:" + "a1" * 32).to_str(False)
NOMINATOR_1 = Address("0:" + "b2" * 32).to_str(False)
NOMINATOR_2 = Address("0:" + "c3" * 32).to_str(False)

_EMPTY_BODY = begin_cell().end_cell().to_boc()


# --- synthetic trace construction (mirrors test_dns.py) ---------------------

def _tx(hash_: str, lt: int, account: str, aborted: bool = False) -> Transaction:
    tx = Transaction(
        hash=hash_, lt=lt, account=account, now=1000 + lt, total_fees=0,
        orig_status="active", end_status="active", descr="ord",
        aborted=aborted, emulated=False,
    )
    tx.messages = []
    return tx


def _msg(msg_hash, tx, direction, opcode, src, dst, body_boc, value=10 ** 9, bounced=False) -> Message:
    m = Message(
        msg_hash=msg_hash, tx_hash=tx.hash, tx_lt=tx.lt, direction=direction,
        source=src, destination=dst, value=value, fwd_fee=0, ihr_fee=0,
        created_lt=tx.lt, created_at=tx.now, opcode=opcode, bounce=False,
        bounced=bounced, value_extra_currencies=None,
    )
    m.message_content = MessageContent(hash="c", body=body_boc)
    m.transaction = tx
    return m


def _build_withdraw_transactions(payout_values: list[int], *, payout_aborted: bool = False) -> list[Transaction]:
    """A NominatorPoolProcessWithdrawRequests call to the pool, sending one
    ton_transfer payout per value in `payout_values`, each to its own
    nominator account (each payout needs its own receiving transaction -
    to_tree() asserts every destination-carrying out-message resolves to a
    real tx)."""
    root_tx = _tx("TXROOT", 100, POOL_ADDR)
    call_msg = _msg("CALL", root_tx, "in", WITHDRAW_OPCODE, None, POOL_ADDR, _EMPTY_BODY, value=None)

    payout_msgs = []
    payout_txs = []
    nominators = [NOMINATOR_1, NOMINATOR_2]
    for i, value in enumerate(payout_values):
        nominator = nominators[i]
        msg_hash = f"PAYOUT{i}"
        payout_tx = _tx(f"TXPAYOUT{i}", 200 + i * 100, nominator, aborted=payout_aborted)
        payout_msgs.append(
            _msg(msg_hash, root_tx, "out", None, POOL_ADDR, nominator, _EMPTY_BODY, value=value)
        )
        payout_tx.messages = [
            _msg(msg_hash, payout_tx, "in", None, POOL_ADDR, nominator, _EMPTY_BODY, value=value)
        ]
        payout_txs.append(payout_tx)

    root_tx.messages = [call_msg] + payout_msgs
    return [root_tx] + payout_txs


def _interfaces(is_nominator_pool: bool = True) -> dict:
    if not is_nominator_pool:
        return {}
    return {POOL_ADDR: {"NominatorPool": {}}}


# --- harness (mirrors test_dns.py) ------------------------------------------

def _compile_dsl_matcher():
    compiled = compile_file(NOMINATOR_MCH, nominator_registries())
    return compiled[0].matcher


def _build_block_tree(txs) -> Block:
    node = to_tree(txs)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matcher(matcher, txs) -> list:
    root = _build_block_tree(txs)
    matches: list = []
    for b in root.bfs_iter():
        if b.btype == "root":
            continue
        if b.parent is not None:
            continue
        result = await matcher.try_build(b)
        if result is not None:
            matches.extend(result)
    return matches


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


def _assert_field_parity(actual: list[Block], expected: list[Block],
                          actual_tag: str, expected_tag: str) -> None:
    assert len(actual) == len(expected), (
        f"count mismatch: {actual_tag}={len(actual)} {expected_tag}={len(expected)}"
    )
    for a, e in zip(sorted(actual, key=_sort_key), sorted(expected, key=_sort_key)):
        assert a.btype == e.btype == "nominator_pool_withdraw_request"
        assert a.failed == e.failed, f"failed flag: {actual_tag}={a.failed} {expected_tag}={e.failed}"
        assert a.broken == e.broken, f"broken flag: {actual_tag}={a.broken} {expected_tag}={e.broken}"

        assert type(a.data) is type(e.data) is NominatorPoolWithdrawRequestData
        for fname in ("source", "pool", "payout_amount"):
            av, ev = getattr(a.data, fname), getattr(e.data, fname)
            assert _values_equal(av, ev), (
                f"data.{fname}: {actual_tag}={av!r} ({type(av).__name__}) "
                f"{expected_tag}={ev!r} ({type(ev).__name__})"
            )

        assert a.value_flow.to_dict() == e.value_flow.to_dict(), (
            f"value_flow: {actual_tag}={a.value_flow.to_dict()} "
            f"{expected_tag}={e.value_flow.to_dict()}"
        )
        assert _node_hashes(a) == _node_hashes(e), (
            f"consumed event nodes: {actual_tag}={_node_hashes(a)} "
            f"{expected_tag}={_node_hashes(e)}"
        )


async def _run_parity(txs, *, interfaces: dict) -> tuple[list[Block], list[Block]]:
    """legacy vs compiled, then compiled vs IR round trip."""
    context.interface_repository.set(TestInterfaceRepository(interfaces))
    hw_blocks = await _run_matcher(NominatorPoolWithdrawMatcher(), txs)
    context.interface_repository.set(TestInterfaceRepository(interfaces))
    compiled = compile_file(NOMINATOR_MCH, nominator_registries())
    by_name = {cm.name: cm.matcher for cm in compiled}
    dsl_blocks = await _run_matcher(by_name["nominator_pool_withdraw"], txs)
    _assert_field_parity(dsl_blocks, hw_blocks, "dsl", "legacy")

    artifact = emit_ir(compiled, [NOMINATOR_MCH.name])
    render_json(artifact)
    if not _HAVE_IR:
        pytest.skip(_SKIP_REASON)

    art = _mch_ir.load_artifact(artifact)
    ir_by_name = {m.name: m for m in _mch_ir.build_matchers(art, nominator_registries())}
    context.interface_repository.set(TestInterfaceRepository(interfaces))
    ir_blocks = await _run_matcher(ir_by_name["nominator_pool_withdraw"], txs)
    _assert_field_parity(ir_blocks, dsl_blocks, "ir", "compiled")
    return hw_blocks, dsl_blocks


@pytest.mark.asyncio
async def test_single_payout_parity():
    txs = _build_withdraw_transactions([5 * 10 ** 9])
    hw_blocks, _ = await _run_parity(txs, interfaces=_interfaces())
    assert len(hw_blocks) == 1
    assert hw_blocks[0].data.payout_amount.value == 5 * 10 ** 9


@pytest.mark.asyncio
async def test_two_payouts_parity_same_granularity():
    """R05 accept criterion: output granularity (block count) must match
    legacy exactly - one block per payout, not one aggregate block."""
    txs = _build_withdraw_transactions([5 * 10 ** 9, 7 * 10 ** 9])
    hw_blocks, dsl_blocks = await _run_parity(txs, interfaces=_interfaces())
    assert len(hw_blocks) == 2
    assert len(dsl_blocks) == 2


@pytest.mark.asyncio
async def test_rejects_non_nominator_pool():
    """Both paths must reject identically when the call target has no
    NominatorPool interface - legacy's outer check vs the DSL's per-payout
    check in the shared core produce the same (empty) result."""
    txs = _build_withdraw_transactions([5 * 10 ** 9, 7 * 10 ** 9])
    hw_blocks, dsl_blocks = await _run_parity(txs, interfaces=_interfaces(is_nominator_pool=False))
    assert hw_blocks == []
    assert dsl_blocks == []


@pytest.mark.asyncio
async def test_failed_flag_never_set_even_on_aborted_payout_tx():
    """Legacy quirk pinned: NominatorPoolWithdrawMatcher never sets
    new_block.failed (unlike its sibling matchers in the same file) - it
    stays the Block-default False regardless of the payout tx's abort
    status. The remake must reproduce this exactly, not "fix" it."""
    txs = _build_withdraw_transactions([5 * 10 ** 9], payout_aborted=True)
    hw_blocks, dsl_blocks = await _run_parity(txs, interfaces=_interfaces())
    assert hw_blocks[0].failed is False
    assert dsl_blocks[0].failed is False
