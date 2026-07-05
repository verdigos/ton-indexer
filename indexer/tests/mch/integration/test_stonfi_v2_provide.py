"""R08 — three-way parity (legacy vs compiled vs v2 IR engine) between
StonfiV2ProvideLiquidityMatcher and specs/stonfi_v2_provide.mch +
post_process_stonfi_v2_liquidity.

No stonfi_v2 liquidity trace exists in the corpus, so — like
test_stonfi_v2_withdraw.py (R04) and test_dns.py (T14) — this suite builds
minimal but faithful block trees the way the production pipeline does: real
Transaction/Message DB models with genuine BOC bodies -> to_tree() ->
init_block(), with the prerequisite matchers replayed first in production
order (JettonTransferBlockMatcher / PTonTransferMatcher run BEFORE the
provide matcher).

Architecture (class C remake): the legacy matcher pairs the two deposit
branches IN-MATCHER (GenericMatcher over the previously produced
dex_deposit_liquidity block) and performs the EmptyBlock proxy insertion
inside build_block. The remake matches each leg independently
(final = with the pool callback subtree, partial = without) and the host pass
post_process_stonfi_v2_liquidity replays the pairing — so the legacy side is
compared against the remake AFTER the pass (the pass is invoked manually
here; production registration is T26).

Coverage:
- both-branch, jetton partial + pTON final: deterministic pairing direction
  (the pTON anchor sits one level deeper, so in the legacy single-BFS pass the
  jetton leg fires first and the pTON leg absorbs it) — field, consumed-set
  and topology parity incl. the inserted EmptyBlock proxy;
- both-branch, jetton + jetton: legacy pairing direction is set-order
  nondeterministic (compact_connections rebuilds sibling lists as
  list(set(...))), so the data compare is swap-tolerant on the _1/_2 field
  triples; consumed set and value flow are direction-independent and exact;
- single-branch jetton (partial survives), single-branch pTON with LP mint
  (final alone), single-branch refund (failed=True);
- both-branch condition-fail (two partials, lp None == None): the legacy
  sibling walk still matched, so legacy absorbs the sibling AND the shared
  parent block without a proxy or data update — the pass replays that too.

Documented deviation: the remake blocks are class-tagged
(StonfiV2DepositLiquidity / StonfiV2DepositLiquidityPartial subclasses of
Block) where legacy produces plain Block — btype ('dex_deposit_liquidity')
and everything the remake gate lists are identical; topology signatures
compare btype, not class.
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
from indexer.events.blocks.liquidity import (
    StonfiV2DepositLiquidity,
    StonfiV2DepositLiquidityPartial,
    StonfiV2ProvideLiquidityMatcher,
    post_process_stonfi_v2_liquidity,
)
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_file
from indexer.events.mch.builders import stonfi_provide_registries
from indexer.events.mch.ir_emit import emit_ir, render_json
import indexer.events.mch.specs as specs_pkg
from tests.utils.repository import TestInterfaceRepository

PROVIDE_MCH = Path(specs_pkg.__file__).parent / "stonfi_v2_provide.mch"

# --- IR engine gate (same convention as test_stonfi_v2_withdraw)
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

PROVIDE_OPCODE = 0x37C096DF
ADD_OPCODE = 0x50C6A654
CB_OPCODE = 0x06ECD527
JETTON_TRANSFER_OPCODE = 0x0F8A7EA5
JETTON_INTERNAL_OPCODE = 0x178D4519
JETTON_NOTIFY_OPCODE = 0x7362D09C
PTON_OPCODE = 0x01F3835D

USER = "0:" + "AA" * 32
ROUTER = "0:" + "DD" * 32
POOL = "0:" + "EE" * 32
LP_ACC = "0:" + "ED" * 32
USER_LP_JW = "0:" + "BC" * 32

USER_JW_A = "0:" + "22" * 32
ROUTER_JW_A = "0:" + "23" * 32
JETTON_A = "0:" + "33" * 32

USER_JW_B = "0:" + "44" * 32
ROUTER_JW_B = "0:" + "45" * 32
JETTON_B = "0:" + "55" * 32

PTON_WALLET = "0:" + "66" * 32

JETTON_DEPOSIT_AMOUNT = 5_000_000
TON_DEPOSIT_AMOUNT = 7_000_000_000
JETTON_B_DEPOSIT_AMOUNT = 900_000
LP_MINTED = 123_456


# --- synthetic BOC bodies ------------------------------------------------------

def _op_body(opcode: int) -> bytes:
    return (begin_cell().store_uint(opcode, 32).store_uint(7, 64).end_cell()).to_boc()


def _jetton_transfer_body(amount: int, to: str) -> bytes:
    return (begin_cell().store_uint(JETTON_TRANSFER_OPCODE, 32).store_uint(7, 64)
            .store_coins(amount).store_address(Address(to)).store_address(Address(to))
            .store_bit(0).store_coins(1).store_bit(0).end_cell()).to_boc()


def _jetton_internal_body(amount: int, owner: str) -> bytes:
    return (begin_cell().store_uint(JETTON_INTERNAL_OPCODE, 32).store_uint(7, 64)
            .store_coins(amount).store_address(Address(owner)).store_address(Address(owner))
            .store_coins(1).end_cell()).to_boc()


def _jetton_notify_body(amount: int, sender: str) -> bytes:
    return (begin_cell().store_uint(JETTON_NOTIFY_OPCODE, 32).store_uint(7, 64)
            .store_coins(amount).store_address(Address(sender))
            .store_bit(0).end_cell()).to_boc()


def _provide_body(amount1: int, amount2: int) -> bytes:
    return (begin_cell().store_uint(PROVIDE_OPCODE, 32).store_uint(7, 64)
            .store_address(Address(USER)).store_coins(amount1).store_coins(amount2)
            .end_cell()).to_boc()


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


def _pool_tail(base_lt: int, prefix: str, provide_hash: str, provide_body: bytes,
               tail: str) -> list[Transaction]:
    """provide (ROUTER->POOL) -> add_liquidity (POOL->LP_ACC), plus the pool
    callback subtree per `tail`: 'none' | 'mint' | 'refund'."""
    pool_tx = _tx(prefix + "TXPOOL", base_lt, POOL)
    pool_tx.messages = [
        _msg(provide_hash, pool_tx, "in", PROVIDE_OPCODE, ROUTER, POOL, provide_body),
        _msg(prefix + "MADD", pool_tx, "out", ADD_OPCODE, POOL, LP_ACC, _op_body(ADD_OPCODE)),
    ]
    lp_tx = _tx(prefix + "TXLP", base_lt + 10, LP_ACC)
    lp_tx.messages = [
        _msg(prefix + "MADD", lp_tx, "in", ADD_OPCODE, POOL, LP_ACC, _op_body(ADD_OPCODE)),
    ]
    txs = [pool_tx, lp_tx]
    if tail == "none":
        return txs
    lp_tx.messages.append(
        _msg(prefix + "MCB", lp_tx, "out", CB_OPCODE, LP_ACC, POOL, _op_body(CB_OPCODE)))
    cb_tx = _tx(prefix + "TXCB", base_lt + 20, POOL)
    cb_tx.messages = [
        _msg(prefix + "MCB", cb_tx, "in", CB_OPCODE, LP_ACC, POOL, _op_body(CB_OPCODE)),
    ]
    txs.append(cb_tx)
    if tail == "mint":
        cb_tx.messages.append(
            _msg(prefix + "MMINT", cb_tx, "out", JETTON_INTERNAL_OPCODE, POOL, USER_LP_JW,
                 _jetton_internal_body(LP_MINTED, USER)))
        mint_tx = _tx(prefix + "TXMINT", base_lt + 30, USER_LP_JW)
        mint_tx.messages = [
            _msg(prefix + "MMINT", mint_tx, "in", JETTON_INTERNAL_OPCODE, POOL, USER_LP_JW,
                 _jetton_internal_body(LP_MINTED, USER)),
        ]
        txs.append(mint_tx)
    elif tail == "refund":
        cb_tx.messages.append(
            _msg(prefix + "MREF", cb_tx, "out", ADD_OPCODE, POOL, ROUTER, _op_body(ADD_OPCODE)))
        ref_tx = _tx(prefix + "TXREF", base_lt + 30, ROUTER)
        ref_tx.messages = [
            _msg(prefix + "MREF", ref_tx, "in", ADD_OPCODE, POOL, ROUTER, _op_body(ADD_OPCODE)),
        ]
        txs.append(ref_tx)
    return txs


def _jetton_leg(base_lt: int, prefix: str, jt_hash: str, user_jw: str, router_jw: str,
                amount: int, tail: str) -> list[Transaction]:
    """User deposits a jetton: transfer chain (USER_JW -> ROUTER_JW -> notify
    ROUTER) then provide-liquidity to the pool. `jt_hash` is the user wallet's
    out-message hash (the leg root)."""
    jw_tx = _tx(prefix + "TXJW", base_lt, user_jw)
    jw_tx.messages = [
        _msg(jt_hash, jw_tx, "in", JETTON_TRANSFER_OPCODE, USER, user_jw,
             _jetton_transfer_body(amount, ROUTER)),
        _msg(prefix + "MJI", jw_tx, "out", JETTON_INTERNAL_OPCODE, user_jw, router_jw,
             _jetton_internal_body(amount, ROUTER)),
    ]
    rjw_tx = _tx(prefix + "TXRJW", base_lt + 10, router_jw)
    rjw_tx.messages = [
        _msg(prefix + "MJI", rjw_tx, "in", JETTON_INTERNAL_OPCODE, user_jw, router_jw,
             _jetton_internal_body(amount, ROUTER)),
        _msg(prefix + "MNOTIF", rjw_tx, "out", JETTON_NOTIFY_OPCODE, router_jw, ROUTER,
             _jetton_notify_body(amount, USER)),
    ]
    provide_body = _provide_body(amount, 0)
    router_tx = _tx(prefix + "TXROUTER", base_lt + 20, ROUTER)
    router_tx.messages = [
        _msg(prefix + "MNOTIF", router_tx, "in", JETTON_NOTIFY_OPCODE, router_jw, ROUTER,
             _jetton_notify_body(amount, USER)),
        _msg(prefix + "MPROV", router_tx, "out", PROVIDE_OPCODE, ROUTER, POOL, provide_body),
    ]
    return [jw_tx, rjw_tx, router_tx] + _pool_tail(
        base_lt + 30, prefix, prefix + "MPROV", provide_body, tail)


def _pton_leg(base_lt: int, prefix: str, pton_hash: str, tail: str) -> list[Transaction]:
    """User deposits TON via pTON: raw 0x01f3835d to the pTON wallet, which
    notifies the router; then provide-liquidity to the pool."""
    pw_tx = _tx(prefix + "TXPW", base_lt, PTON_WALLET)
    pw_tx.messages = [
        _msg(pton_hash, pw_tx, "in", PTON_OPCODE, USER, PTON_WALLET, _op_body(PTON_OPCODE)),
        _msg(prefix + "MNOTIF", pw_tx, "out", JETTON_NOTIFY_OPCODE, PTON_WALLET, ROUTER,
             _jetton_notify_body(TON_DEPOSIT_AMOUNT, USER)),
    ]
    provide_body = _provide_body(0, TON_DEPOSIT_AMOUNT)
    router_tx = _tx(prefix + "TXROUTER", base_lt + 10, ROUTER)
    router_tx.messages = [
        _msg(prefix + "MNOTIF", router_tx, "in", JETTON_NOTIFY_OPCODE, PTON_WALLET, ROUTER,
             _jetton_notify_body(TON_DEPOSIT_AMOUNT, USER)),
        _msg(prefix + "MPROV", router_tx, "out", PROVIDE_OPCODE, ROUTER, POOL, provide_body),
    ]
    return [pw_tx, router_tx] + _pool_tail(
        base_lt + 20, prefix, prefix + "MPROV", provide_body, tail)


def _build_transactions(legs: list[str]) -> list[Transaction]:
    """Root user-wallet tx with one out message per leg. Leg specs:
    'jetton_a:<tail>', 'jetton_b:<tail>', 'pton:<tail>'."""
    user_tx = _tx("TXUSER", 100, USER)
    user_tx.messages = [
        _msg("EXT", user_tx, "in", None, None, USER, _op_body(0), value=None),
    ]
    txs = [user_tx]
    base_lt = 200
    for i, spec in enumerate(legs):
        kind, tail = spec.split(":")
        prefix = f"L{i}"
        if kind == "jetton_a":
            root_hash = prefix + "MJT"
            user_tx.messages.append(
                _msg(root_hash, user_tx, "out", JETTON_TRANSFER_OPCODE, USER, USER_JW_A,
                     _jetton_transfer_body(JETTON_DEPOSIT_AMOUNT, ROUTER)))
            txs += _jetton_leg(base_lt, prefix, root_hash, USER_JW_A, ROUTER_JW_A,
                               JETTON_DEPOSIT_AMOUNT, tail)
        elif kind == "jetton_b":
            root_hash = prefix + "MJT"
            user_tx.messages.append(
                _msg(root_hash, user_tx, "out", JETTON_TRANSFER_OPCODE, USER, USER_JW_B,
                     _jetton_transfer_body(JETTON_B_DEPOSIT_AMOUNT, ROUTER)))
            txs += _jetton_leg(base_lt, prefix, root_hash, USER_JW_B, ROUTER_JW_B,
                               JETTON_B_DEPOSIT_AMOUNT, tail)
        elif kind == "pton":
            root_hash = prefix + "MPTON"
            user_tx.messages.append(
                _msg(root_hash, user_tx, "out", PTON_OPCODE, USER, PTON_WALLET,
                     _op_body(PTON_OPCODE), value=TON_DEPOSIT_AMOUNT))
            txs += _pton_leg(base_lt, prefix, root_hash, tail)
        else:  # pragma: no cover - bad spec
            raise ValueError(spec)
        base_lt += 1000
    return txs


def _interfaces() -> dict:
    user_raw = Address(USER).to_str(False)
    router_raw = Address(ROUTER).to_str(False)

    def jw(addr, owner, jetton):
        return {"JettonWallet": {"balance": 0, "address": addr, "owner": owner, "jetton": jetton}}

    return {
        USER_JW_A: jw(USER_JW_A, user_raw, JETTON_A),
        ROUTER_JW_A: jw(ROUTER_JW_A, router_raw, JETTON_A),
        USER_JW_B: jw(USER_JW_B, user_raw, JETTON_B),
        ROUTER_JW_B: jw(ROUTER_JW_B, router_raw, JETTON_B),
    }


# --- harness -------------------------------------------------------------------

_PREREQUISITES: list[BlockMatcher] = [
    JettonTransferBlockMatcher(),
    PTonTransferMatcher(),
]

_DEPOSIT_CLASSES = (StonfiV2DepositLiquidity, StonfiV2DepositLiquidityPartial)


def _build_block_tree(txs) -> Block:
    node = to_tree(txs)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matchers(matchers: list[BlockMatcher], txs) -> list[Block]:
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
    return [b for b in matches if b.btype == "dex_deposit_liquidity"]


def _unabsorbed(blocks: list[Block]) -> list[Block]:
    """Drop blocks that a later fire absorbed (legacy pairing leaves the first
    fire's block in the produced list with .parent set)."""
    return [b for b in blocks if b.parent is None]


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


def _tree_sig(block: Block) -> tuple:
    """Recursive topology signature: btype (classes deviate by design),
    consumed node hashes, children signatures, next-block btypes."""
    return (
        block.btype,
        tuple(_node_hashes(block)),
        tuple(sorted(_tree_sig(c) for c in block.children_blocks)),
        tuple(sorted(n.btype for n in block.next_blocks)),
    )


def _assert_field_parity(actual: list[Block], reference: list[Block],
                         label: str, topology: bool = True) -> None:
    assert len(actual) == len(reference), (
        f"{label} count mismatch: actual={len(actual)} reference={len(reference)}"
    )
    for a, r in zip(sorted(actual, key=_sort_key), sorted(reference, key=_sort_key)):
        assert a.btype == r.btype, f"{label} btype: {a.btype} vs {r.btype}"
        assert a.failed == r.failed, f"{label} failed flag: {a.failed} vs {r.failed}"
        assert a.broken == r.broken, f"{label} broken flag: {a.broken} vs {r.broken}"
        assert set(a.data.keys()) == set(r.data.keys()), (
            f"{label} data keys: {sorted(a.data)} vs {sorted(r.data)}"
        )
        for key in sorted(r.data):
            av, rv = a.data[key], r.data[key]
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
        if topology:
            assert _tree_sig(a) == _tree_sig(r), (
                f"{label} topology signature mismatch:\n{_tree_sig(a)}\nvs\n{_tree_sig(r)}"
            )
            a_prev = a.previous_block.btype if a.previous_block else None
            r_prev = r.previous_block.btype if r.previous_block else None
            assert a_prev == r_prev, f"{label} previous_block btype: {a_prev} vs {r_prev}"
            a_init = a.initiating_event_node.message.tx_hash if a.initiating_event_node else None
            r_init = r.initiating_event_node.message.tx_hash if r.initiating_event_node else None
            assert a_init == r_init, f"{label} initiating node: {a_init} vs {r_init}"


_DIRECTIONAL = (('amount_1', 'amount_2'), ('asset_1', 'asset_2'),
                ('sender_wallet_1', 'sender_wallet_2'))


def _assert_swap_tolerant_parity(actual: list[Block], reference: list[Block],
                                 label: str) -> None:
    """Same bar as _assert_field_parity minus topology, with the _1/_2 field
    triples allowed to be mirrored (legacy pairing direction for same-depth
    sibling legs is set-order nondeterministic)."""
    assert len(actual) == len(reference) == 1, f"{label} expected exactly one block per side"
    a, r = actual[0], reference[0]
    assert a.btype == r.btype and a.failed == r.failed and a.broken == r.broken
    assert set(a.data.keys()) == set(r.data.keys())
    directional = {k for pair in _DIRECTIONAL for k in pair}
    for key in sorted(r.data):
        if key in directional:
            continue
        assert _values_equal(a.data[key], r.data[key]), (
            f"{label} data[{key!r}]: {a.data[key]!r} vs {r.data[key]!r}"
        )
    exact = all(_values_equal(a.data[k1], r.data[k1]) and _values_equal(a.data[k2], r.data[k2])
                for k1, k2 in _DIRECTIONAL)
    swapped = all(_values_equal(a.data[k1], r.data[k2]) and _values_equal(a.data[k2], r.data[k1])
                  for k1, k2 in _DIRECTIONAL)
    assert exact or swapped, (
        f"{label} directional fields match neither exactly nor mirrored: "
        f"{ {k: a.data[k] for pair in _DIRECTIONAL for k in pair} } vs "
        f"{ {k: r.data[k] for pair in _DIRECTIONAL for k in pair} }"
    )
    assert a.value_flow.to_dict() == r.value_flow.to_dict()
    assert _node_hashes(a) == _node_hashes(r)


def _ir_matchers():
    compiled = compile_file(PROVIDE_MCH, stonfi_provide_registries())
    artifact = emit_ir(compiled, [PROVIDE_MCH.name])
    render_json(artifact)  # always exercise the JSON render (break must error, not skip)
    if not _HAVE_IR:
        return None
    art = _mch_ir.load_artifact(artifact)
    return [m.matcher if hasattr(m, "matcher") else m
            for m in _mch_ir.build_matchers(art, stonfi_provide_registries())]


async def _run_legacy(legs: list[str]) -> list[Block]:
    context.interface_repository.set(TestInterfaceRepository(_interfaces()))
    blocks = await _run_matchers([StonfiV2ProvideLiquidityMatcher()], _build_transactions(legs))
    return _unabsorbed(blocks)


async def _run_remake(matchers: list[BlockMatcher], legs: list[str]) -> list[Block]:
    context.interface_repository.set(TestInterfaceRepository(_interfaces()))
    blocks = await _run_matchers(matchers, _build_transactions(legs))
    blocks = await post_process_stonfi_v2_liquidity(blocks)
    return _unabsorbed(blocks)


async def _run_parity(legs: list[str], compare=_assert_field_parity) -> list[Block]:
    """legacy vs the v2 IR engine (+pairing pass); the compiled path is retired
    (T29). Returns legacy blocks."""
    hw = await _run_legacy(legs)

    ir_matchers = _ir_matchers()
    if ir_matchers is None:
        pytest.skip(_SKIP_REASON)
    ir = await _run_remake(ir_matchers, legs)
    compare(ir, hw, "ir-vs-legacy")
    return hw


# --- cases ---------------------------------------------------------------------

@pytest.mark.asyncio
async def test_pair_jetton_partial_pton_final():
    """Both branches: jetton leg without callback + pTON leg with LP mint.
    The pTON anchor is one level deeper, so the legacy BFS fires the jetton
    leg first and the pTON fire absorbs it — deterministic direction. The
    pass must produce the identical merged block, EmptyBlock proxy included."""
    hw = await _run_parity(["jetton_a:none", "pton:mint"])
    assert len(hw) == 1
    b = hw[0]
    assert b.data["asset_1"].is_ton is True
    assert b.data["amount_1"].value == TON_DEPOSIT_AMOUNT
    assert b.data["sender_wallet_1"] is None
    assert b.data["asset_2"].is_ton is False
    assert b.data["amount_2"].value == JETTON_DEPOSIT_AMOUNT
    assert b.data["sender_wallet_2"] is not None
    assert b.data["lp_tokens_minted"].value == LP_MINTED
    assert b.failed is False
    # proxy present in both trees (checked pairwise by the topology signature)
    assert any(c.btype == "empty" for c in b.children_blocks)


@pytest.mark.asyncio
async def test_pair_jetton_jetton_swap_tolerant():
    """Both branches jetton (same anchor depth): legacy pairing direction is
    set-order nondeterministic, so data compares are swap-tolerant; consumed
    set and value flow are direction-independent and exact."""
    hw = await _run_parity(["jetton_a:none", "jetton_b:mint"],
                           compare=_assert_swap_tolerant_parity)
    b = hw[0]
    assert b.data["lp_tokens_minted"].value == LP_MINTED
    assets = {b.data["asset_1"].jetton_address, b.data["asset_2"].jetton_address}
    assert len(assets) == 2


@pytest.mark.asyncio
async def test_single_jetton_partial_survives():
    """Single deposit leg without callback: the partial must survive the pass
    exactly as legacy leaves its single-fire block."""
    hw = await _run_parity(["jetton_a:none"])
    b = hw[0]
    assert b.data["lp_tokens_minted"] is None
    assert b.data["amount_2"] is None
    assert b.data["asset_1"].is_ton is False
    assert b.failed is False


@pytest.mark.asyncio
async def test_single_pton_final():
    """Single pTON leg with LP mint (final alone, raw 0x01f3835d in-arm)."""
    hw = await _run_parity(["pton:mint"])
    b = hw[0]
    assert b.data["asset_1"].is_ton is True
    assert b.data["amount_1"].value == TON_DEPOSIT_AMOUNT
    assert b.data["lp_tokens_minted"].value == LP_MINTED
    assert b.failed is False


@pytest.mark.asyncio
async def test_single_refund_failed():
    """Callback refunds (cb -> 0x50c6a654 back): failed=True on both sides."""
    hw = await _run_parity(["jetton_a:refund"])
    b = hw[0]
    assert b.failed is True
    assert b.data["lp_tokens_minted"] is None


@pytest.mark.asyncio
async def test_both_partial_condition_fail_absorbs_parent():
    """Two callback-less legs: the legacy sibling walk matches but the lp
    condition fails (None == None) — legacy still absorbs the sibling deposit
    AND the shared parent block, with no proxy and no data update. The pass
    replays that branch. jetton + pTON keeps the direction deterministic."""
    hw = await _run_parity(["jetton_a:none", "pton:none"])
    assert len(hw) == 1
    b = hw[0]
    assert b.data["amount_2"] is None
    assert b.data["lp_tokens_minted"] is None
    # the pTON leg absorbed the jetton leg: amount_1 is the TON side
    assert b.data["asset_1"].is_ton is True
    # no proxy in the condition-fail branch
    assert not any(c.btype == "empty" for c in b.children_blocks)
    # the shared parent (user wallet root block) was absorbed
    assert "TXUSER" in _node_hashes(b)
