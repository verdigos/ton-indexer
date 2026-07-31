"""Registries for specs/dedust_v2_claim_reward.mch (T12.2 — first ABI-path protocol).

The C++ engine parses these three message bodies through the ton-abi generated
structs + the AbiValue->mch::Value adapter (bridge). The Python mch twin has no
ABI parser, so it needs hand parsers registered under the SAME bare names the C++
`abi_message_parsers()` rows use (ClaimRewardMessage / PayoutRewardMessage /
PayoutMessage). To stay diff-0 with the adapter output these classes are
TOLK-FAITHFUL: camelCase attribute names matching the ABI fields, coins/uintN ->
plain int (mirrors adapter Int), address -> AccountId (mirrors adapter Account,
read UNWRAPPED in the spec), maybe-ref cell -> Cell|None. See
ton-mch-engine/T122-CLAIMREWARD-DESIGN.md §1.

OP-GUARD RULE (binding for every ABI-path parser, not just these): the C++ side
CHECKS the struct's opcode prefix inside `from_slice` and fails the parse on a
mismatch, while these Python twins skip the leading 32 bits unchecked (the
pytoniq convention the rest of the engine follows). The two sides therefore only
agree when the body is known to carry the right opcode — so an ABI-path `parse`
capture MUST sit behind an `op` guard in the .mch spec. An unguarded ABI parse
yields a C++ error and a Python garbage object on the same body.

ADAPTER SHAPE RECIPE (what `abi_value_to_mch` produces per AbiValueKind, i.e.
what a Python twin attribute has to look like; full rationale in
ton-mch-engine/W12-BRIDGE-DESIGN.md §3):

    Int / enum        -> Int              int
    Bool              -> Bool             bool
    Address (Std)     -> Account          AccountId(addr)   (uppercase raw render)
    Address (None)    -> Account, addr_none=True            AccountId(None)
    Address (Extern)  -> Obj{bits, value} dict(bits=int, value=Cell)
    Cell              -> Cell             pytoniq Cell
    CellOf            -> Obj{ref}         dict(ref=<inner>)
    Bits N            -> Cell             the bits packed into a Cell
    String            -> Str (RAW bytes; no UTF-8 decode on this path)
    List              -> List             list  (arrayOf/lispListOf/tensor/tuple)
    Struct            -> Obj{"$": <StructName>, <fields in decl order>}
    Union has_value   -> Obj{"$": label, "value": <inner>}
    Union struct-lbl  -> the inner Struct itself (it already carries its own "$")
    Void              -> Obj{"$": "void"}
    Null              -> Null             None
    Map               -> List of [k, v] pairs in wire order (NOT a dict — keys
                                          may be ints/addresses)

`ABI_FIELDS` below pins each class's attribute names AND order against the
committed ABI; tests/mch/test_abi_twin_fields.py enforces it.
"""
from __future__ import annotations

import base64
from types import SimpleNamespace
from typing import Any, Awaitable, Callable

from pytoniq_core import Cell, Slice

from indexer.events.blocks.core import Block
from indexer.events.blocks.dedust_v2.swaps import _final_min_out
from indexer.events.blocks.messages import (
    DedustV2ClaimAvailableFees,
    DedustV2ClaimPositionFees,
    DedustV2CreditAsset,
    DedustV2CreditLiquidity,
    DedustV2DepositEvent,
    DedustV2DepositPayload,
    DedustV2ExitLiquidity,
    DedustV2JoinLiquidity,
    DedustV2PayJetton,
    DedustV2PayoutPositionFees,
    DedustV2SwapPayload,
    DedustV2Withdraw,
    DedustV2WithdrawalEvent,
    DedustV2WithdrawLiquidity,
)
from indexer.events.blocks.swaps import JettonSwapBlock
from indexer.events.blocks.utils import AccountId
from indexer.events.mch.builders.dedust import default_lookups
from indexer.events.mch.registry import Registries


class ClaimRewardMessage:
    # claim_reward#909fdb65 query_id:uint64 reward_index:uint2 excesses_to:MsgAddressInt
    opcode = 0x909FDB65

    def __init__(self, slice: Slice):
        slice.load_uint(32)  # opcode (checked by the matcher's `op`)
        self.queryId = slice.load_uint(64)
        self.rewardIndex = slice.load_uint(2)
        self.excessesTo = AccountId(slice.load_address())


class PayoutRewardMessage:
    # payout_reward#9e17fbbe query_id:uint64 amount:Coins reward_index:uint2
    #                        owner_address:MsgAddressInt excesses_to:MsgAddressInt
    opcode = 0x9E17FBBE

    def __init__(self, slice: Slice):
        slice.load_uint(32)
        self.queryId = slice.load_uint(64)
        self.amount = slice.load_coins()
        self.rewardIndex = slice.load_uint(2)
        self.ownerAddress = AccountId(slice.load_address())
        self.excessesTo = AccountId(slice.load_address())


class PayoutMessage:
    # payout#3216ca09 query_id:uint64 amount:Coins exit_code:int32 payload:(Maybe ^Cell)
    opcode = 0x3216CA09

    def __init__(self, slice: Slice):
        slice.load_uint(32)
        self.queryId = slice.load_uint(64)
        self.amount = slice.load_coins()
        self.exitCode = slice.load_int(32)
        self.payload = slice.load_maybe_ref()


# Attribute names, in ABI declaration order, for each twin class. The C++ adapter
# emits struct fields in decl order under exactly these keys, so a rename or a
# reorder on either side silently breaks the twin — pinned here and checked
# against ton-abi/abi/dedust_v2.abi.json by tests/mch/test_abi_twin_fields.py.
ABI_FIELDS = {
    "ClaimRewardMessage": ("queryId", "rewardIndex", "excessesTo"),
    "PayoutRewardMessage": ("queryId", "amount", "rewardIndex", "ownerAddress", "excessesTo"),
    "PayoutMessage": ("queryId", "amount", "exitCode", "payload"),
}

CLAIM_REWARD_OPCODES = {
    "ClaimRewardMessage": ClaimRewardMessage.opcode,
    "ClaimPositionReward": 0x2E0CCCBA,  # matched by opcode only (body never parsed)
    "PayoutRewardMessage": PayoutRewardMessage.opcode,
    "PayoutMessage": PayoutMessage.opcode,
}

CLAIM_REWARD_MESSAGE_TYPES = {
    "ClaimRewardMessage": ClaimRewardMessage,
    "PayoutRewardMessage": PayoutRewardMessage,
    "PayoutMessage": PayoutMessage,
}

CLAIM_REWARD_BLOCK_TYPES = {
    "dedust_v2_claim_reward": Block,
}


def register_dedust_v2_claim_reward(reg: Registries) -> Registries:
    reg.opcodes.update(CLAIM_REWARD_OPCODES)
    reg.block_types.update(CLAIM_REWARD_BLOCK_TYPES)
    reg.message_types.update(CLAIM_REWARD_MESSAGE_TYPES)
    reg.lookups.update(default_lookups())
    return reg


def dedust_v2_claim_reward_registries() -> Registries:
    """Fresh Registries ready to compile specs/dedust_v2_claim_reward.mch."""
    return register_dedust_v2_claim_reward(Registries())


# ---------------------------------------------------------------------------
# specs/dedust_v2_claim_fees.mch — the LP trading-fees claim, sibling of the
# reward claim above.
#
# MIXED parser paths: PayoutMessage is the ABI-bridge twin from this module
# (camelCase), while DedustV2PayoutPositionFees rides the mch-msgs.schema entry
# of the same name — and its Python twin is the LEGACY class itself. Reusing the
# legacy class (rather than writing a schema-shaped twin like the swap family
# does) is safe here and strictly better for the sweep: the fields the spec reads
# are spelled identically on both sides, and the class's `assert op == opcode`
# only ever sees the right body because the spec guards the capture with
# `op DedustV2PayoutPositionFees`.
# ---------------------------------------------------------------------------
CLAIM_FEES_OPCODES = {
    "ClaimPositionFees": DedustV2ClaimPositionFees.opcode,
    "ClaimAvailableFees": DedustV2ClaimAvailableFees.opcode,  # matched by opcode only
    "DedustV2PayoutPositionFees": DedustV2PayoutPositionFees.opcode,
    "PayoutMessage": PayoutMessage.opcode,
}

CLAIM_FEES_MESSAGE_TYPES = {
    "DedustV2PayoutPositionFees": DedustV2PayoutPositionFees,
    "PayoutMessage": PayoutMessage,
}


def register_dedust_v2_claim_fees(reg: Registries) -> Registries:
    reg.opcodes.update(CLAIM_FEES_OPCODES)
    reg.block_types["dedust_v2_claim_fees"] = Block
    reg.message_types.update(CLAIM_FEES_MESSAGE_TYPES)
    reg.lookups.update(default_lookups())
    return reg


def dedust_v2_claim_fees_registries() -> Registries:
    """Fresh Registries ready to compile specs/dedust_v2_claim_fees.mch."""
    return register_dedust_v2_claim_fees(Registries())


# ---------------------------------------------------------------------------
# specs/dedust_v2_withdraw.mch — the liquidity withdrawal.
#
# Same mixed-path arrangement as claim_fees above: PayoutMessage is the ABI twin
# from this module, while DedustV2Withdraw / DedustV2WithdrawalEvent ride their
# mch-msgs.schema entries. The EVENT reuses the legacy class as its twin
# (identical field spellings, and the spec guards the capture with an `op` so
# the class's `assert op == opcode` only ever sees the right body).
#
# The ENTRY message cannot: it carries the schema's first `bool` leaf, and the
# legacy class reads it with pytoniq's `Slice.load_bit()`, which returns an
# **int** (0/1). The C++ leaf emits a real Bool, and the expression VM faults
# on a non-bool condition — so `reject when withdraw.body.auto_claim_fees`
# passes on one engine and blows up on the other. Hence the schema-shaped twin
# below, whose only job is the `bool(...)` cast. THE RULE FOR EVERY FUTURE
# `bool` FIELD: the Python twin must yield a real bool, never load_bit()'s int.
#
# Unlike the swap family's schema twins, this one still parses the message to
# its END (it stops nowhere early), because the schema entry does too — that is
# what makes both sides fail on exactly the same truncated bodies.
#
# WithdrawLiquidity 0xbc1531a9 and ExitLiquidity 0xf6f6a3aa are pattern-only:
# they need an opcode row but no parser, since neither body is ever read.
# ---------------------------------------------------------------------------
class DedustV2WithdrawTwin:
    """withdraw#20b5ef89 — schema twin of mch-msgs.schema `DedustV2Withdraw`.

    Registered under the schema entry's name; the class name differs only to
    avoid shadowing the legacy import above."""

    opcode = DedustV2Withdraw.opcode

    def __init__(self, slice: Slice):
        slice.load_uint(32)  # opcode (checked by the matcher's `op`)
        slice.load_uint(64)  # query_id, parsed-and-dropped
        self.liquidity = slice.load_coins()
        slice.load_coins()  # minimal_x_out, parsed-and-dropped
        slice.load_coins()  # minimal_y_out, parsed-and-dropped
        self.auto_claim_fees = bool(slice.load_bit())
        slice.load_ref()  # payout_config, parsed-and-dropped


WITHDRAW_OPCODES = {
    "DedustV2Withdraw": DedustV2Withdraw.opcode,
    "DedustV2WithdrawLiquidity": DedustV2WithdrawLiquidity.opcode,  # opcode only
    "DedustV2ExitLiquidity": DedustV2ExitLiquidity.opcode,          # opcode only
    "DedustV2WithdrawalEvent": DedustV2WithdrawalEvent.opcode,
    "PayoutMessage": PayoutMessage.opcode,
}

WITHDRAW_MESSAGE_TYPES = {
    "DedustV2Withdraw": DedustV2WithdrawTwin,
    "DedustV2WithdrawalEvent": DedustV2WithdrawalEvent,
    "PayoutMessage": PayoutMessage,
}


def register_dedust_v2_withdraw(reg: Registries) -> Registries:
    reg.opcodes.update(WITHDRAW_OPCODES)
    reg.block_types["dex_withdraw_liquidity"] = Block
    reg.message_types.update(WITHDRAW_MESSAGE_TYPES)
    reg.lookups.update(default_lookups())
    return reg


def dedust_v2_withdraw_registries() -> Registries:
    """Fresh Registries ready to compile specs/dedust_v2_withdraw.mch."""
    return register_dedust_v2_withdraw(Registries())


# ---------------------------------------------------------------------------
# specs_decl/dedust_v2_swap.mch — the CPMM V2 swap family.
#
# These two message twins mirror the mch-msgs.schema entries of the same name
# (the C++ side's generated parsers), NOT the legacy blocks/messages/dedust_v2.py
# classes: only the fields the spec reads are parsed, and the leading opcode is
# skipped unchecked exactly like every schema-generated parser.
# ---------------------------------------------------------------------------
class DedustV2PayNative:
    """pay_native#a5a7cbf8 query_id:uint64 amount:Coins payment_payload:^Cell …

    query_id and payout_config are not read by the spec, so they are dropped /
    left unparsed (the schema twin stops after payment_payload)."""

    opcode = 0xA5A7CBF8

    def __init__(self, slice: Slice):
        slice.load_uint(32)  # opcode (checked by the matcher's `op`)
        slice.load_uint(64)  # query_id, parsed-and-dropped
        self.amount = slice.load_coins()
        self.payment_payload = slice.load_ref()


class DedustV2SwapEvent:
    """swap#78e79ba4 x_to_y:Bool amount_in:Coins amount_out:Coins …

    The pool's external-out log: the only exact source of the per-hop amounts.
    x_to_y and everything after amount_out are not read by the spec."""

    opcode = 0x78E79BA4

    def __init__(self, slice: Slice):
        slice.load_uint(32)
        slice.load_bit()  # x_to_y, parsed-and-dropped
        self.amount_in = slice.load_coins()
        self.amount_out = slice.load_coins()


async def dedust_v2_swap_payload(payload: Any) -> SimpleNamespace | None:
    """The carrier's swap payload, or None when the carrier is not a swap.

    Port of `_swap_payload` + `_final_min_out` (blocks/dedust_v2/swaps.py), with
    the two carrier shapes the DSL can reach folded into one argument:

      - Cell — a PayNative body's `payment_payload` ref (TON carrier);
      - str  — a jetton_transfer block's b64 `forward_payload`, i.e. the
               PayJetton wrapper whose own `payment_payload` holds the payload.

    None mirrors the reference's "not a swap payload" (wrong opcode or an
    unparseable payload). `min_out` is the FINAL minimal_amount_out: the entry
    payload only carries hop 1's, the last SwapStep of the route carries the
    real floor, so the `next` chain is walked (same depth cap as the reference).
    A payload that parses but whose step chain does not yields min_out=None,
    where the reference's jetton arm would raise — the only deviation.
    """
    if payload is None:
        return None
    try:
        if isinstance(payload, str):
            wrapper = DedustV2PayJetton(Cell.one_from_boc(base64.b64decode(payload)).begin_parse())
            if wrapper.payment_payload_opcode != DedustV2SwapPayload.opcode:
                return None
            payload_cell = wrapper.payment_payload
        else:
            payload_cell = payload
        if payload_cell.to_slice().load_uint(32) != DedustV2SwapPayload.opcode:
            return None
        parsed = DedustV2SwapPayload(payload_cell.begin_parse())
    except Exception:
        return None
    try:
        min_out = _final_min_out(parsed).value
    except Exception:
        min_out = None
    return SimpleNamespace(min_out=min_out)


def dedust_pool_lookup() -> dict[str, Callable[..., Awaitable[Any]]]:
    """`lookup dedust_pool(addr)` — the pool's asset pair.

    The C++ side serves this kind from the block's own interfaces (tier 1); here
    it is the same interface repository the reference matcher reads through
    `_pool_other_asset`. Accepts either an AccountId (a `.data` address field) or
    a raw string, since both reach the lookup from the spec.
    """
    from indexer.events import context

    async def dedust_pool(address: Any) -> Any:
        repo = context.interface_repository.get()
        if repo is None:
            return None
        key = address.as_str() if isinstance(address, AccountId) else address.upper()
        return await repo.get_dedust_pool(key)

    return {"dedust_pool": dedust_pool}


SWAP_OPCODES = {
    "DedustV2PayNative": DedustV2PayNative.opcode,
    "DedustV2SwapEvent": DedustV2SwapEvent.opcode,
    "PayoutMessage": PayoutMessage.opcode,
}

SWAP_MESSAGE_TYPES = {
    "DedustV2PayNative": DedustV2PayNative,
    "DedustV2SwapEvent": DedustV2SwapEvent,
    "PayoutMessage": PayoutMessage,
}


def dedust_v2_swap_registries() -> Registries:
    """Fresh Registries ready to compile specs_decl/dedust_v2_swap.mch."""
    reg = Registries()
    reg.opcodes.update(SWAP_OPCODES)
    reg.block_types["JettonSwapBlock"] = JettonSwapBlock
    reg.message_types.update(SWAP_MESSAGE_TYPES)
    reg.lookups.update(default_lookups())
    reg.lookups.update(dedust_pool_lookup())
    reg.fns["dedust_v2_swap_payload"] = dedust_v2_swap_payload
    return reg


# ---------------------------------------------------------------------------
# specs/dedust_v2_deposit.mch — the deposit trio (final / rejected / partial).
#
# Three schema twins, all mirroring mch-msgs.schema entries of the same name
# (not the legacy blocks/messages/dedust_v2.py classes): the leading opcode is
# skipped unchecked, and — like the withdraw entry — each parses to the END of
# the wire shape so both engines fail on exactly the same truncations.
#
# `is_x` is the second user of the schema's `bool` leaf, so DedustV2CreditAsset
# obeys the CASTING-TWIN RULE stated above the withdraw twin: pytoniq's
# `load_bit()` yields an int, the C++ leaf yields a real Bool, and the
# expression VM faults on a non-bool used as a condition — hence `bool(...)`.
#
# DedustV2CreditLiquidity 0x855afcbd is pattern-only (an opcode row, no parser):
# the reference never reads its body either.
# ---------------------------------------------------------------------------
class DedustV2CreditAssetTwin:
    """credit_asset#dc5ddba1 — schema twin of `DedustV2CreditAsset`."""

    opcode = DedustV2CreditAsset.opcode

    def __init__(self, slice: Slice):
        slice.load_uint(32)  # opcode (checked by the matcher's `op`)
        slice.load_uint(64)  # query_id, parsed-and-dropped
        self.is_x = bool(slice.load_bit())  # casting twin (see header)
        self.amount = slice.load_coins()


class DedustV2JoinLiquidityTwin:
    """join_liquidity#25251ee0 — schema twin of `DedustV2JoinLiquidity`."""

    opcode = DedustV2JoinLiquidity.opcode

    def __init__(self, slice: Slice):
        slice.load_uint(32)
        slice.load_uint(64)  # query_id, parsed-and-dropped
        slice.load_ref()  # config, parsed-and-dropped
        self.amount_x = slice.load_coins()
        self.amount_y = slice.load_coins()
        self.initiator = slice.load_address()


class DedustV2DepositEventTwin:
    """deposit#35df2e12 — schema twin of `DedustV2DepositEvent`."""

    opcode = DedustV2DepositEvent.opcode

    def __init__(self, slice: Slice):
        slice.load_uint(32)
        self.amount_x_in = slice.load_coins()
        self.amount_y_in = slice.load_coins()
        self.liquidity = slice.load_coins()
        slice.load_address()  # initiator, parsed-and-dropped
        slice.load_address()  # recipient, parsed-and-dropped
        slice.load_ref()  # next_state, parsed-and-dropped


async def dedust_v2_deposit_payload(payload: Any) -> SimpleNamespace | None:
    """The carrier's DEPOSIT payload (the desired amount_x/amount_y), or None.

    Port of `_deposit_payload` (blocks/dedust_v2/liquidity.py:180), with the two
    carrier shapes the DSL can reach folded into one argument — the same
    arrangement as `dedust_v2_swap_payload` above:

      - Cell — a PayNative body's `payment_payload` ref (TON carrier);
      - str  — a jetton_transfer block's b64 `forward_payload`, i.e. the
               PayJetton wrapper whose own `payment_payload` holds the payload.

    None mirrors the reference's "not a deposit payload" (a missing carrier
    payload, the wrong opcode — PayNative also carries swaps and fund rewards —
    or an unparseable body), which drives both target amounts to null.
    """
    if payload is None:
        return None
    try:
        if isinstance(payload, str):
            wrapper = DedustV2PayJetton(Cell.one_from_boc(base64.b64decode(payload)).begin_parse())
            payload_cell = wrapper.payment_payload
        else:
            payload_cell = payload
        if payload_cell.to_slice().load_uint(32) != DedustV2DepositPayload.opcode:
            return None
        parsed = DedustV2DepositPayload(payload_cell.begin_parse())
    except Exception:
        return None
    return SimpleNamespace(amount_x=parsed.amount_x, amount_y=parsed.amount_y)


DEPOSIT_OPCODES = {
    "DedustV2PayNative": DedustV2PayNative.opcode,
    "DedustV2CreditAsset": DedustV2CreditAsset.opcode,
    "DedustV2JoinLiquidity": DedustV2JoinLiquidity.opcode,
    "DedustV2DepositEvent": DedustV2DepositEvent.opcode,
    "DedustV2CreditLiquidity": DedustV2CreditLiquidity.opcode,  # opcode only
    "PayoutMessage": PayoutMessage.opcode,
}

DEPOSIT_MESSAGE_TYPES = {
    "DedustV2PayNative": DedustV2PayNative,
    "DedustV2CreditAsset": DedustV2CreditAssetTwin,
    "DedustV2JoinLiquidity": DedustV2JoinLiquidityTwin,
    "DedustV2DepositEvent": DedustV2DepositEventTwin,
    "PayoutMessage": PayoutMessage,
}

DEPOSIT_BLOCK_TYPES = {
    "dedust_v2_deposit_liquidity": Block,
    "dedust_v2_deposit_liquidity_partial": Block,
}


def register_dedust_v2_deposit(reg: Registries) -> Registries:
    reg.opcodes.update(DEPOSIT_OPCODES)
    reg.block_types.update(DEPOSIT_BLOCK_TYPES)
    reg.message_types.update(DEPOSIT_MESSAGE_TYPES)
    reg.lookups.update(default_lookups())
    reg.fns["dedust_v2_deposit_payload"] = dedust_v2_deposit_payload
    return reg


def dedust_v2_deposit_registries() -> Registries:
    """Fresh Registries ready to compile specs/dedust_v2_deposit.mch."""
    return register_dedust_v2_deposit(Registries())
