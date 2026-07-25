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

from pytoniq_core import Slice

from indexer.events.blocks.core import Block
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
