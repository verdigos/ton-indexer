"""T17 — field-level parity between DedustWithdrawBlockMatcher and
specs/dedust_withdraw.mch.

Like test_dns.py (T14), this suite does NOT load recorded trace fixtures: no
trace in tests/traces contains both the JettonBurn (0x595f07bc) and
DedustPayoutFromPool (0xad4eb6f5) opcodes — verified by scanning every
fixture's raw message opcodes and by running the legacy matcher across all 147
traces (0 hits); no YAML case covers a dedust dex_withdraw_liquidity either.
Recording new traces is out of scope for T17, so minimal but faithful block
trees are built the same way the production pipeline does: real
Transaction/Message DB models with genuine BOC bodies -> to_tree() ->
init_block(), prerequisite jetton matchers replayed first (production
registration order: JettonTransfer/PTon run before DedustWithdraw, and
DedustWithdraw runs BEFORE JettonBurnBlockMatcher, so the burn anchor is
untouched).

Identical-heads children set: the two children items share the head
`op DedustPayoutFromPool`. The engine consumes first-match-wins in candidate
order, so payout_1 = first matching payout chain, payout_2 = the next — the
mixed fixtures pin that the capture assignment matches legacy
labeled('payout_1')/labeled('payout_2') semantics in both orders (jetton
first / TON first).

Coverage:
- jetton payout first + TON payout second (mixed, assignment pinned)
- TON payout first + jetton payout second (mirrored, assignment pinned)
- reject path: LP jetton wallet missing from the interface repo — the shared
  core returns None and BOTH matchers reject identically (spec §10.7)
"""
from __future__ import annotations

from pathlib import Path

import pytest
from pytoniq_core import Address, begin_cell

from indexer.core.database import Message, MessageContent, Transaction
from indexer.events import context
from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.jettons import (
    JettonTransferBlockMatcher,
    PTonTransferMatcher,
)
from indexer.events.blocks.liquidity import DedustWithdrawBlockMatcher
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_file
from indexer.events.mch.builders import dedust_withdraw_registries
import indexer.events.mch.specs as specs_pkg
from tests.utils.repository import TestInterfaceRepository

WITHDRAW_MCH = Path(specs_pkg.__file__).parent / "dedust_withdraw.mch"

BURN_OPCODE = 0x595F07BC
BURN_NOTIF_OPCODE = 0x7BDD97DE
PAYOUT_FROM_POOL_OPCODE = 0xAD4EB6F5
PAYOUT_OPCODE = 0x474F86CF
JETTON_TRANSFER_OPCODE = 0x0F8A7EA5
JETTON_INTERNAL_OPCODE = 0x178D4519

SENDER = "0:" + "AA" * 32
LP_WALLET = "0:" + "BB" * 32
LP_MASTER = "0:" + "CC" * 32
POOL = "0:" + "DD" * 32
VAULT_TON = "0:" + "EE" * 32
VAULT_JET = "0:" + "FF" * 32
VAULT_JW = "0:" + "11" * 32
USER_JW = "0:" + "22" * 32
JETTON_A = "0:" + "33" * 32

LP_TOKENS_BURNT = 5_000_000
TON_PAYOUT_AMOUNT = 7_000_000_000
JETTON_PAYOUT_AMOUNT = 900_000


# --- synthetic BOC bodies ------------------------------------------------------

def _burn_body() -> bytes:
    return (begin_cell().store_uint(BURN_OPCODE, 32).store_uint(7, 64)
            .store_coins(LP_TOKENS_BURNT).store_address(Address(SENDER)).end_cell()).to_boc()


def _burn_notif_body() -> bytes:
    return (begin_cell().store_uint(BURN_NOTIF_OPCODE, 32).store_uint(7, 64)
            .store_coins(LP_TOKENS_BURNT).end_cell()).to_boc()


def _payout_request_body(amount: int) -> bytes:
    proof = begin_cell().end_cell()
    return (begin_cell().store_uint(PAYOUT_FROM_POOL_OPCODE, 32).store_uint(7, 64)
            .store_ref(proof).store_coins(amount).end_cell()).to_boc()


def _ton_payout_body(amount: int) -> bytes:
    proof = begin_cell().end_cell()
    return (begin_cell().store_uint(PAYOUT_OPCODE, 32).store_uint(7, 64)
            .store_ref(proof).store_coins(amount).end_cell()).to_boc()


def _jetton_transfer_body(amount: int) -> bytes:
    return (begin_cell().store_uint(JETTON_TRANSFER_OPCODE, 32).store_uint(7, 64)
            .store_coins(amount).store_address(Address(SENDER)).store_address(Address(VAULT_JET))
            .store_bit(0).store_coins(0).store_bit(0).end_cell()).to_boc()


def _jetton_internal_body(amount: int) -> bytes:
    return (begin_cell().store_uint(JETTON_INTERNAL_OPCODE, 32).store_uint(7, 64)
            .store_coins(amount).store_address(Address(VAULT_JET)).store_address(Address(VAULT_JET))
            .store_coins(0).end_cell()).to_boc()


# --- synthetic trace construction (mirrors the real pipeline path) -------------

def _tx(hash_: str, lt: int, account: str) -> Transaction:
    tx = Transaction(
        hash=hash_, lt=lt, account=account, now=1000 + lt, total_fees=0,
        orig_status="active", end_status="active", descr="ord",
        aborted=False, emulated=False,
    )
    tx.messages = []
    return tx


def _msg(msg_hash, tx, direction, opcode, src, dst, body_boc, value=10 ** 9) -> Message:
    m = Message(
        msg_hash=msg_hash, tx_hash=tx.hash, tx_lt=tx.lt, direction=direction,
        source=src, destination=dst, value=value, fwd_fee=0, ihr_fee=0,
        created_lt=tx.lt, created_at=tx.now, opcode=opcode, bounce=False,
        bounced=False, value_extra_currencies=None,
    )
    m.message_content = MessageContent(hash="c", body=body_boc)
    m.transaction = tx
    return m


def _ton_payout_leg(vault_tx_lt: int, request_hash: str, payout_hash: str) -> list[Transaction]:
    vault_tx = _tx("TX_" + request_hash, vault_tx_lt, VAULT_TON)
    vault_tx.messages = [
        _msg(request_hash, vault_tx, "in", PAYOUT_FROM_POOL_OPCODE, POOL, VAULT_TON,
             _payout_request_body(TON_PAYOUT_AMOUNT)),
        _msg(payout_hash, vault_tx, "out", PAYOUT_OPCODE, VAULT_TON, SENDER,
             _ton_payout_body(TON_PAYOUT_AMOUNT)),
    ]
    recv_tx = _tx("TX_" + payout_hash, vault_tx_lt + 100, SENDER)
    recv_tx.messages = [
        _msg(payout_hash, recv_tx, "in", PAYOUT_OPCODE, VAULT_TON, SENDER,
             _ton_payout_body(TON_PAYOUT_AMOUNT)),
    ]
    return [vault_tx, recv_tx]


def _jetton_payout_leg(vault_tx_lt: int, request_hash: str, prefix: str) -> list[Transaction]:
    vault_tx = _tx("TX_" + request_hash, vault_tx_lt, VAULT_JET)
    vault_tx.messages = [
        _msg(request_hash, vault_tx, "in", PAYOUT_FROM_POOL_OPCODE, POOL, VAULT_JET,
             _payout_request_body(JETTON_PAYOUT_AMOUNT)),
        _msg(prefix + "JT", vault_tx, "out", JETTON_TRANSFER_OPCODE, VAULT_JET, VAULT_JW,
             _jetton_transfer_body(JETTON_PAYOUT_AMOUNT)),
    ]
    jw_tx = _tx(prefix + "TXJW", vault_tx_lt + 100, VAULT_JW)
    jw_tx.messages = [
        _msg(prefix + "JT", jw_tx, "in", JETTON_TRANSFER_OPCODE, VAULT_JET, VAULT_JW,
             _jetton_transfer_body(JETTON_PAYOUT_AMOUNT)),
        _msg(prefix + "JI", jw_tx, "out", JETTON_INTERNAL_OPCODE, VAULT_JW, USER_JW,
             _jetton_internal_body(JETTON_PAYOUT_AMOUNT)),
    ]
    user_jw_tx = _tx(prefix + "TXUJW", vault_tx_lt + 200, USER_JW)
    user_jw_tx.messages = [
        _msg(prefix + "JI", user_jw_tx, "in", JETTON_INTERNAL_OPCODE, VAULT_JW, USER_JW,
             _jetton_internal_body(JETTON_PAYOUT_AMOUNT)),
    ]
    return [vault_tx, jw_tx, user_jw_tx]


def _build_withdraw_transactions(jetton_first: bool) -> list[Transaction]:
    """Sender burns LP tokens; the pool pays out one jetton leg and one TON
    leg. `jetton_first` controls the pool's out-message order, i.e. which leg
    the children set consumes as payout_1."""
    wallet_tx = _tx("TXROOT", 100, SENDER)
    wallet_tx.messages = [
        _msg("EXT", wallet_tx, "in", None, None, SENDER, _burn_notif_body(), value=None),
        _msg("MBURN", wallet_tx, "out", BURN_OPCODE, SENDER, LP_WALLET, _burn_body()),
    ]
    lp_tx = _tx("TXLP", 200, LP_WALLET)
    lp_tx.messages = [
        _msg("MBURN", lp_tx, "in", BURN_OPCODE, SENDER, LP_WALLET, _burn_body()),
        _msg("MNOTIF", lp_tx, "out", BURN_NOTIF_OPCODE, LP_WALLET, POOL, _burn_notif_body()),
    ]
    pool_tx = _tx("TXPOOL", 300, POOL)
    jet_request = _msg("MREQJET", pool_tx, "out", PAYOUT_FROM_POOL_OPCODE, POOL, VAULT_JET,
                       _payout_request_body(JETTON_PAYOUT_AMOUNT))
    ton_request = _msg("MREQTON", pool_tx, "out", PAYOUT_FROM_POOL_OPCODE, POOL, VAULT_TON,
                       _payout_request_body(TON_PAYOUT_AMOUNT))
    pool_tx.messages = [
        _msg("MNOTIF", pool_tx, "in", BURN_NOTIF_OPCODE, LP_WALLET, POOL, _burn_notif_body()),
        *((jet_request, ton_request) if jetton_first else (ton_request, jet_request)),
    ]

    txs = [wallet_tx, lp_tx, pool_tx]
    txs += _jetton_payout_leg(400, "MREQJET", "J1")
    txs += _ton_payout_leg(450, "MREQTON", "MPAYOUT")
    return txs


def _interfaces(with_lp_wallet: bool = True) -> dict:
    sender_raw_lower = Address(SENDER).to_str(False)
    interfaces = {
        USER_JW: {
            "JettonWallet": {
                "balance": 0, "address": USER_JW,
                "owner": sender_raw_lower, "jetton": JETTON_A,
            }
        },
    }
    if with_lp_wallet:
        interfaces[LP_WALLET] = {
            "JettonWallet": {
                "balance": 0, "address": LP_WALLET,
                "owner": sender_raw_lower, "jetton": LP_MASTER,
            }
        }
    return interfaces


# --- harness (mirrors test_dedust_swap.py / test_dns.py) -----------------------

_PREREQUISITES: list[BlockMatcher] = [
    JettonTransferBlockMatcher(),
    PTonTransferMatcher(),
]


def _compile_dsl_matcher() -> BlockMatcher:
    compiled = compile_file(WITHDRAW_MCH, dedust_withdraw_registries())
    by_name = {cm.name: cm.matcher for cm in compiled}
    return by_name["dedust_withdraw"]


def _build_block_tree(txs) -> Block:
    node = to_tree(txs)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matcher(matcher: BlockMatcher, txs) -> list:
    root = _build_block_tree(txs)
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
    return [b for b in matches if b.btype == "dex_withdraw_liquidity"]


def _values_equal(a, b) -> bool:
    if a is None and b is None:
        return True
    if type(a) is not type(b):
        return False
    return bool(a == b)


def _node_hashes(block: Block) -> list[str]:
    return sorted(n.message.tx_hash for n in block.event_nodes)


def _assert_field_parity(dsl_blocks: list[Block], hw_blocks: list[Block]) -> None:
    assert len(hw_blocks) == 1, f"legacy matcher produced {len(hw_blocks)} blocks - bad fixture"
    assert len(dsl_blocks) == len(hw_blocks), (
        f"count mismatch: dsl={len(dsl_blocks)} legacy={len(hw_blocks)}"
    )
    for d, h in zip(dsl_blocks, hw_blocks):
        assert d.btype == h.btype == "dex_withdraw_liquidity"
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


async def _run_parity(jetton_first: bool):
    context.interface_repository.set(TestInterfaceRepository(_interfaces()))
    hw_blocks = await _run_matcher(
        DedustWithdrawBlockMatcher(), _build_withdraw_transactions(jetton_first))
    context.interface_repository.set(TestInterfaceRepository(_interfaces()))
    dsl_blocks = await _run_matcher(
        _compile_dsl_matcher(), _build_withdraw_transactions(jetton_first))
    _assert_field_parity(dsl_blocks, hw_blocks)
    return hw_blocks, dsl_blocks


@pytest.mark.asyncio
async def test_dedust_withdraw_parity_jetton_first():
    """Mixed payouts, jetton leg first in candidate order: both matchers must
    assign payout_1 = jetton, payout_2 = TON (identical-heads children set,
    first-match-wins)."""
    hw_blocks, dsl_blocks = await _run_parity(jetton_first=True)
    for b in (hw_blocks[0], dsl_blocks[0]):
        assert b.data["asset1_out"].is_ton is False
        assert b.data["amount1_out"].value == JETTON_PAYOUT_AMOUNT
        assert b.data["asset2_out"].is_ton is True
        assert b.data["amount2_out"].value == TON_PAYOUT_AMOUNT


@pytest.mark.asyncio
async def test_dedust_withdraw_parity_ton_first():
    """Mirrored candidate order: payout_1 = TON, payout_2 = jetton — pins that
    the deterministic assignment follows candidate order, not payout type."""
    hw_blocks, dsl_blocks = await _run_parity(jetton_first=False)
    for b in (hw_blocks[0], dsl_blocks[0]):
        assert b.data["asset1_out"].is_ton is True
        assert b.data["amount1_out"].value == TON_PAYOUT_AMOUNT
        assert b.data["asset2_out"].is_ton is False
        assert b.data["amount2_out"].value == JETTON_PAYOUT_AMOUNT


@pytest.mark.asyncio
async def test_dedust_withdraw_both_reject_without_lp_wallet():
    """LP jetton wallet absent from the interface repo: the shared build core
    returns None, and per spec §10.7 both matchers must reject (consume
    nothing) rather than abort."""
    context.interface_repository.set(TestInterfaceRepository(_interfaces(with_lp_wallet=False)))
    hw_blocks = await _run_matcher(
        DedustWithdrawBlockMatcher(), _build_withdraw_transactions(jetton_first=True))
    context.interface_repository.set(TestInterfaceRepository(_interfaces(with_lp_wallet=False)))
    dsl_blocks = await _run_matcher(
        _compile_dsl_matcher(), _build_withdraw_transactions(jetton_first=True))
    assert hw_blocks == []
    assert dsl_blocks == []
