"""R04 — three-way parity (legacy vs compiled vs v2 IR engine) between
StonfiV2WithdrawLiquidityMatcher and specs/stonfi_v2_withdraw.mch.

No stonfi_v2 liquidity trace exists in the corpus (no YAML case, no fixture),
so — like test_dedust_withdraw.py (T17) and test_dns.py (T14) — this suite
builds minimal but faithful block trees the way the production pipeline does:
real Transaction/Message DB models with genuine BOC bodies -> to_tree() ->
init_block(), with the prerequisite matchers replayed first in production order
(JettonTransfer / PTon / JettonBurn all run BEFORE the withdraw matcher, so the
pay-to jetton_transfer blocks and the jetton_burn anchor are prebuilt).

Shape: user burns LP tokens; the pool pays out both pool assets as two pay-to
(op 0x657b54f5) siblings under the router (op 0x297437cf), each with a
jetton_transfer child; a TON payout carries a PTonTransfer (0x01f3835d)
grandchild the legacy build reads the ton_amount off. The legacy matcher anchors
on one pay-to and imperatively scans siblings for the other; the remake makes
the two pay-tos a children set (@payout_1/@payout_2, first-match-wins in
candidate order) exactly like dedust_withdraw, so the assignment matches legacy
in both payout orders.

Refund branch (legacy OrMatcher parent arm): no burn — anchored on op 0x0f98e2b8
with its parent op 0x132b9a2c on a `<-` edge; the core reads the sender off
refund.previous_block. Split into a second matcher because the two branches
share no structural head.

Coverage:
- jetton payout first + TON payout second (assignment pinned)
- TON payout first + jetton payout second (mirrored assignment pinned)
- refund withdraw (is_refund=True, sender from the refund parent)
- reject path: LP jetton wallet missing from the interface repo — the jetton
  burn prerequisite can't build, so no jetton_burn anchor exists and BOTH
  legacy + remake produce nothing.
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
    JettonBurnBlockMatcher,
    JettonTransferBlockMatcher,
    PTonTransferMatcher,
)
from indexer.events.blocks.liquidity import StonfiV2WithdrawLiquidityMatcher
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_file
from indexer.events.mch.builders import stonfi_liquidity_registries
from indexer.events.mch.ir_emit import emit_ir, render_json
import indexer.events.mch.specs as specs_pkg
from tests.utils.repository import TestInterfaceRepository

WITHDRAW_MCH = Path(specs_pkg.__file__).parent / "stonfi_v2_withdraw.mch"

# --- IR engine gate (same convention as test_evaa_withdraw / test_ir_engine_parity)
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

BURN_OPCODE = 0x595F07BC
BURN_NOTIF_OPCODE = 0x7BDD97DE
ROUTER_OPCODE = 0x297437CF
REFUND_OPCODE = 0x0F98E2B8
REFUND_PARENT_OPCODE = 0x132B9A2C
PAYTO_OPCODE = 0x657B54F5
PTON_OPCODE = 0x01F3835D
JETTON_TRANSFER_OPCODE = 0x0F8A7EA5
JETTON_INTERNAL_OPCODE = 0x178D4519

USER = "0:" + "AA" * 32
LP_WALLET = "0:" + "BB" * 32
LP_MASTER = "0:" + "CC" * 32
ROUTER = "0:" + "DD" * 32
POOL = "0:" + "EE" * 32
REFUND_CTR = "0:" + "DE" * 32

VAULT_JET = "0:" + "FF" * 32
VAULT_JW = "0:" + "11" * 32
USER_JW = "0:" + "22" * 32
JETTON_A = "0:" + "33" * 32

VAULT_TON = "0:" + "44" * 32
VAULT_TON_JW = "0:" + "55" * 32
USER_PTON_JW = "0:" + "66" * 32
JETTON_B = "0:" + "77" * 32

LP_TOKENS_BURNT = 5_000_000
JETTON_PAYOUT_AMOUNT = 900_000
TON_PAYOUT_AMOUNT = 7_000_000_000


# --- synthetic BOC bodies ------------------------------------------------------

def _burn_body() -> bytes:
    return (begin_cell().store_uint(BURN_OPCODE, 32).store_uint(7, 64)
            .store_coins(LP_TOKENS_BURNT).store_address(Address(USER)).end_cell()).to_boc()


def _burn_notif_body() -> bytes:
    return (begin_cell().store_uint(BURN_NOTIF_OPCODE, 32).store_uint(7, 64)
            .store_coins(LP_TOKENS_BURNT).end_cell()).to_boc()


def _op_body(opcode: int) -> bytes:
    return (begin_cell().store_uint(opcode, 32).store_uint(7, 64).end_cell()).to_boc()


def _jetton_transfer_body(amount: int, to: str) -> bytes:
    return (begin_cell().store_uint(JETTON_TRANSFER_OPCODE, 32).store_uint(7, 64)
            .store_coins(amount).store_address(Address(to)).store_address(Address(to))
            .store_bit(0).store_coins(0).store_bit(0).end_cell()).to_boc()


def _jetton_internal_body(amount: int, owner: str) -> bytes:
    return (begin_cell().store_uint(JETTON_INTERNAL_OPCODE, 32).store_uint(7, 64)
            .store_coins(amount).store_address(Address(owner)).store_address(Address(owner))
            .store_coins(0).end_cell()).to_boc()


def _pton_body(amount: int) -> bytes:
    return (begin_cell().store_uint(PTON_OPCODE, 32).store_uint(7, 64)
            .store_coins(amount).store_address(Address(USER))
            .store_bit(0).end_cell()).to_boc()


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


def _jetton_payout_leg(base_lt: int, payto_hash: str, prefix: str) -> list[Transaction]:
    """Pay-to (POOL->VAULT_JET) received at the vault, which does a plain jetton
    transfer down to the user's jetton wallet."""
    vault_tx = _tx(prefix + "TXV", base_lt, VAULT_JET)
    vault_tx.messages = [
        _msg(payto_hash, vault_tx, "in", PAYTO_OPCODE, POOL, VAULT_JET, _op_body(PAYTO_OPCODE)),
        _msg(prefix + "JT", vault_tx, "out", JETTON_TRANSFER_OPCODE, VAULT_JET, VAULT_JW,
             _jetton_transfer_body(JETTON_PAYOUT_AMOUNT, USER)),
    ]
    jw_tx = _tx(prefix + "TXJW", base_lt + 10, VAULT_JW)
    jw_tx.messages = [
        _msg(prefix + "JT", jw_tx, "in", JETTON_TRANSFER_OPCODE, VAULT_JET, VAULT_JW,
             _jetton_transfer_body(JETTON_PAYOUT_AMOUNT, USER)),
        _msg(prefix + "JI", jw_tx, "out", JETTON_INTERNAL_OPCODE, VAULT_JW, USER_JW,
             _jetton_internal_body(JETTON_PAYOUT_AMOUNT, USER)),
    ]
    user_jw_tx = _tx(prefix + "TXUJW", base_lt + 20, USER_JW)
    user_jw_tx.messages = [
        _msg(prefix + "JI", user_jw_tx, "in", JETTON_INTERNAL_OPCODE, VAULT_JW, USER_JW,
             _jetton_internal_body(JETTON_PAYOUT_AMOUNT, USER)),
    ]
    return [vault_tx, jw_tx, user_jw_tx]


def _ton_payout_leg(base_lt: int, payto_hash: str, prefix: str) -> list[Transaction]:
    """Pay-to (POOL->VAULT_TON) delivered via pTON: a plain jetton transfer whose
    receiver wallet then fires a PTonTransfer (0x01f3835d) carrying the ton
    amount — the leg the legacy build unwraps as a TON payout."""
    vault_tx = _tx(prefix + "TXV", base_lt, VAULT_TON)
    vault_tx.messages = [
        _msg(payto_hash, vault_tx, "in", PAYTO_OPCODE, POOL, VAULT_TON, _op_body(PAYTO_OPCODE)),
        _msg(prefix + "JT", vault_tx, "out", JETTON_TRANSFER_OPCODE, VAULT_TON, VAULT_TON_JW,
             _jetton_transfer_body(TON_PAYOUT_AMOUNT, USER)),
    ]
    jw_tx = _tx(prefix + "TXJW", base_lt + 10, VAULT_TON_JW)
    jw_tx.messages = [
        _msg(prefix + "JT", jw_tx, "in", JETTON_TRANSFER_OPCODE, VAULT_TON, VAULT_TON_JW,
             _jetton_transfer_body(TON_PAYOUT_AMOUNT, USER)),
        _msg(prefix + "JI", jw_tx, "out", JETTON_INTERNAL_OPCODE, VAULT_TON_JW, USER_PTON_JW,
             _jetton_internal_body(TON_PAYOUT_AMOUNT, USER)),
    ]
    user_pton_jw_tx = _tx(prefix + "TXUJW", base_lt + 20, USER_PTON_JW)
    user_pton_jw_tx.messages = [
        _msg(prefix + "JI", user_pton_jw_tx, "in", JETTON_INTERNAL_OPCODE, VAULT_TON_JW, USER_PTON_JW,
             _jetton_internal_body(TON_PAYOUT_AMOUNT, USER)),
        _msg(prefix + "PTON", user_pton_jw_tx, "out", PTON_OPCODE, USER_PTON_JW, USER,
             _pton_body(TON_PAYOUT_AMOUNT)),
    ]
    recv_tx = _tx(prefix + "TXRECV", base_lt + 30, USER)
    recv_tx.messages = [
        _msg(prefix + "PTON", recv_tx, "in", PTON_OPCODE, USER_PTON_JW, USER, _pton_body(TON_PAYOUT_AMOUNT)),
    ]
    return [vault_tx, jw_tx, user_pton_jw_tx, recv_tx]


def _build_withdraw_transactions(jetton_first: bool) -> list[Transaction]:
    """Normal withdraw: user burns LP; router (op 0x297437cf) reaches the pool,
    which pays out one jetton leg and one TON leg. `jetton_first` sets the pool's
    out-message order → which leg the children set consumes as payout_1."""
    user_tx = _tx("TXUSER", 100, USER)
    user_tx.messages = [
        _msg("EXT", user_tx, "in", None, None, USER, _burn_notif_body(), value=None),
        _msg("MBURN", user_tx, "out", BURN_OPCODE, USER, LP_WALLET, _burn_body()),
    ]
    lp_tx = _tx("TXLP", 200, LP_WALLET)
    lp_tx.messages = [
        _msg("MBURN", lp_tx, "in", BURN_OPCODE, USER, LP_WALLET, _burn_body()),
        _msg("MNOTIF", lp_tx, "out", BURN_NOTIF_OPCODE, LP_WALLET, ROUTER, _burn_notif_body()),
    ]
    router_tx = _tx("TXROUTER", 300, ROUTER)
    router_tx.messages = [
        _msg("MNOTIF", router_tx, "in", BURN_NOTIF_OPCODE, LP_WALLET, ROUTER, _burn_notif_body()),
        _msg("MROUTER", router_tx, "out", ROUTER_OPCODE, ROUTER, POOL, _op_body(ROUTER_OPCODE)),
    ]
    pool_tx = _tx("TXPOOL", 400, POOL)
    jet_payto = _msg("PAYJET", pool_tx, "out", PAYTO_OPCODE, POOL, VAULT_JET, _op_body(PAYTO_OPCODE))
    ton_payto = _msg("PAYTON", pool_tx, "out", PAYTO_OPCODE, POOL, VAULT_TON, _op_body(PAYTO_OPCODE))
    pool_tx.messages = [
        _msg("MROUTER", pool_tx, "in", ROUTER_OPCODE, ROUTER, POOL, _op_body(ROUTER_OPCODE)),
        *((jet_payto, ton_payto) if jetton_first else (ton_payto, jet_payto)),
    ]

    txs = [user_tx, lp_tx, router_tx, pool_tx]
    txs += _jetton_payout_leg(500, "PAYJET", "J")
    txs += _ton_payout_leg(600, "PAYTON", "T")
    return txs


def _build_refund_transactions() -> list[Transaction]:
    """Refund withdraw: no burn. The refund parent (op 0x132b9a2c, sent by the
    user) reaches the refund contract, which fires op 0x0f98e2b8 to the pool; the
    pool pays out both legs. Sender is read off the refund parent's source."""
    user_tx = _tx("TXUSER", 100, USER)
    user_tx.messages = [
        _msg("EXT", user_tx, "in", None, None, USER, _burn_notif_body(), value=None),
        _msg("MPARENT", user_tx, "out", REFUND_PARENT_OPCODE, USER, REFUND_CTR,
             _op_body(REFUND_PARENT_OPCODE)),
    ]
    refund_tx = _tx("TXREFUND", 200, REFUND_CTR)
    refund_tx.messages = [
        _msg("MPARENT", refund_tx, "in", REFUND_PARENT_OPCODE, USER, REFUND_CTR,
             _op_body(REFUND_PARENT_OPCODE)),
        _msg("MREFUND", refund_tx, "out", REFUND_OPCODE, REFUND_CTR, POOL, _op_body(REFUND_OPCODE)),
    ]
    pool_tx = _tx("TXPOOL", 300, POOL)
    jet_payto = _msg("PAYJET", pool_tx, "out", PAYTO_OPCODE, POOL, VAULT_JET, _op_body(PAYTO_OPCODE))
    ton_payto = _msg("PAYTON", pool_tx, "out", PAYTO_OPCODE, POOL, VAULT_TON, _op_body(PAYTO_OPCODE))
    pool_tx.messages = [
        _msg("MREFUND", pool_tx, "in", REFUND_OPCODE, REFUND_CTR, POOL, _op_body(REFUND_OPCODE)),
        jet_payto, ton_payto,
    ]

    txs = [user_tx, refund_tx, pool_tx]
    txs += _jetton_payout_leg(500, "PAYJET", "J")
    txs += _ton_payout_leg(600, "PAYTON", "T")
    return txs


def _interfaces(with_lp_wallet: bool = True) -> dict:
    user_raw_lower = Address(USER).to_str(False)
    interfaces = {
        USER_JW: {
            "JettonWallet": {
                "balance": 0, "address": USER_JW,
                "owner": user_raw_lower, "jetton": JETTON_A,
            }
        },
        USER_PTON_JW: {
            "JettonWallet": {
                "balance": 0, "address": USER_PTON_JW,
                "owner": user_raw_lower, "jetton": JETTON_B,
            }
        },
    }
    if with_lp_wallet:
        interfaces[LP_WALLET] = {
            "JettonWallet": {
                "balance": 0, "address": LP_WALLET,
                "owner": user_raw_lower, "jetton": LP_MASTER,
            }
        }
    return interfaces


# --- harness -------------------------------------------------------------------

_PREREQUISITES: list[BlockMatcher] = [
    JettonTransferBlockMatcher(),
    PTonTransferMatcher(),
    JettonBurnBlockMatcher(),
]


def _build_block_tree(txs) -> Block:
    node = to_tree(txs)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matchers(matchers: list[BlockMatcher], txs) -> list:
    root = _build_block_tree(txs)
    for pre in _PREREQUISITES:
        for b in root.bfs_iter():
            if b.parent is None:
                await pre.try_build(b)
    matches: list = []
    for matcher in matchers:
        for b in root.bfs_iter():
            if b.btype == "root" or b.parent is not None:
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


def _assert_field_parity(actual: list[Block], expected: list[Block],
                         actual_tag: str, expected_tag: str) -> None:
    assert len(expected) == 1, f"{expected_tag} produced {len(expected)} blocks - bad fixture"
    assert len(actual) == len(expected), (
        f"count mismatch: {actual_tag}={len(actual)} {expected_tag}={len(expected)}"
    )
    for a, e in zip(actual, expected):
        assert a.btype == e.btype == "dex_withdraw_liquidity"
        assert a.failed == e.failed, f"failed flag: {actual_tag}={a.failed} {expected_tag}={e.failed}"
        assert a.broken == e.broken, f"broken flag: {actual_tag}={a.broken} {expected_tag}={e.broken}"

        assert set(a.data.keys()) == set(e.data.keys()), (
            f"data keys: {actual_tag}={sorted(a.data)} {expected_tag}={sorted(e.data)}"
        )
        for key in sorted(e.data):
            av, ev = a.data[key], e.data[key]
            assert _values_equal(av, ev), (
                f"data[{key!r}]: {actual_tag}={av!r} ({type(av).__name__}) "
                f"{expected_tag}={ev!r} ({type(ev).__name__})"
            )

        assert a.value_flow.to_dict() == e.value_flow.to_dict(), (
            f"value_flow: {actual_tag}={a.value_flow.to_dict()} {expected_tag}={e.value_flow.to_dict()}"
        )
        assert _node_hashes(a) == _node_hashes(e), (
            f"consumed event nodes: {actual_tag}={_node_hashes(a)} {expected_tag}={_node_hashes(e)}"
        )


def _ir_matchers():
    compiled = compile_file(WITHDRAW_MCH, stonfi_liquidity_registries())
    artifact = emit_ir(compiled, [WITHDRAW_MCH.name])
    render_json(artifact)  # always exercise the JSON render (break must error, not skip)
    if not _HAVE_IR:
        return None
    art = _mch_ir.load_artifact(artifact)
    return [m.matcher if hasattr(m, "matcher") else m
            for m in _mch_ir.build_matchers(art, stonfi_liquidity_registries())]


async def _run_parity(build_txs) -> list[Block]:
    """legacy vs the v2 IR engine (the compiled path is retired — T29). Returns
    legacy blocks."""
    context.interface_repository.set(TestInterfaceRepository(_interfaces()))
    hw_blocks = await _run_matchers([StonfiV2WithdrawLiquidityMatcher()], build_txs())

    ir_matchers = _ir_matchers()
    if ir_matchers is None:
        pytest.skip(_SKIP_REASON)
    context.interface_repository.set(TestInterfaceRepository(_interfaces()))
    ir_blocks = await _run_matchers(ir_matchers, build_txs())
    _assert_field_parity(ir_blocks, hw_blocks, "ir", "legacy")
    return hw_blocks


@pytest.mark.asyncio
async def test_stonfi_v2_withdraw_parity_jetton_first():
    """Jetton leg first in candidate order → payout_1 = jetton, payout_2 = TON."""
    hw = await _run_parity(lambda: _build_withdraw_transactions(jetton_first=True))
    b = hw[0]
    assert b.data["is_refund"] is False
    assert b.data["sender"] is not None
    assert b.data["lp_tokens_burnt"].value == LP_TOKENS_BURNT
    assert b.data["asset1_out"].is_ton is False
    assert b.data["amount1_out"].value == JETTON_PAYOUT_AMOUNT
    assert b.data["asset2_out"].is_ton is True
    assert b.data["amount2_out"].value == TON_PAYOUT_AMOUNT


@pytest.mark.asyncio
async def test_stonfi_v2_withdraw_parity_ton_first():
    """Mirrored candidate order → payout_1 = TON, payout_2 = jetton."""
    hw = await _run_parity(lambda: _build_withdraw_transactions(jetton_first=False))
    b = hw[0]
    assert b.data["asset1_out"].is_ton is True
    assert b.data["amount1_out"].value == TON_PAYOUT_AMOUNT
    assert b.data["asset2_out"].is_ton is False
    assert b.data["amount2_out"].value == JETTON_PAYOUT_AMOUNT


@pytest.mark.asyncio
async def test_stonfi_v2_withdraw_parity_refund():
    """Refund branch: no burn, is_refund=True, sender read off the refund parent."""
    hw = await _run_parity(_build_refund_transactions)
    b = hw[0]
    assert b.data["is_refund"] is True
    assert b.data["asset"] is None
    assert b.data["lp_tokens_burnt"] is None
    assert b.data["sender"] is not None


@pytest.mark.asyncio
async def test_stonfi_v2_withdraw_both_reject_without_lp_wallet():
    """LP jetton wallet absent: the jetton_burn prerequisite can't build, so no
    anchor exists and both legacy + remake produce nothing."""
    context.interface_repository.set(TestInterfaceRepository(_interfaces(with_lp_wallet=False)))
    hw_blocks = await _run_matchers(
        [StonfiV2WithdrawLiquidityMatcher()], _build_withdraw_transactions(jetton_first=True))
    ir_matchers = _ir_matchers()
    if ir_matchers is None:
        pytest.skip(_SKIP_REASON)
    context.interface_repository.set(TestInterfaceRepository(_interfaces(with_lp_wallet=False)))
    ir_blocks = await _run_matchers(
        ir_matchers, _build_withdraw_transactions(jetton_first=True))
    assert hw_blocks == []
    assert ir_blocks == []
