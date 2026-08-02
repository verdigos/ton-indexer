"""MCH registries for the Stonfi V2 liquidity matchers.

Both matchers in specs/stonfi_v2_withdraw.mch use declarative builds. The normal
and refund matchers express their produced data with `out {...}` statements, so
there is no host builder here, only opcode and block-type
registration plus the message parsers (`parse pton_* as PTonTransfer`) the
declarative build needs. The shared blocks/liquidity.py build core is still used
by StonfiV2WithdrawLiquidityMatcher but not by the DSL path.
"""
from __future__ import annotations

from indexer.events.blocks.core import Block
from indexer.events.mch.registry import (
    Registries,
    default_lookups,
    default_message_types,
)

OPCODES = {
    "StonfiV2PayTo":                0x657B54F5,
    "StonfiV2WithdrawFromPool":     0x297437CF,
    "StonfiV2WithdrawRefund":       0x0F98E2B8,
    "StonfiV2WithdrawRefundParent": 0x132B9A2C,
    "PTonTransfer":                 0x01F3835D,
}

BLOCK_TYPES = {
    "dex_withdraw_liquidity": Block,
}


def register_stonfi_liquidity(reg: Registries) -> Registries:
    """Register everything specs/stonfi_v2_withdraw.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)
    reg.message_types.update(default_message_types())
    reg.lookups.update(default_lookups())
    return reg


def stonfi_liquidity_registries() -> Registries:
    """Fresh Registries ready to compile specs/stonfi_v2_withdraw.mch."""
    return register_stonfi_liquidity(Registries())
