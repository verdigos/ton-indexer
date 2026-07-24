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
