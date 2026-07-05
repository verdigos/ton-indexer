"""mch registries for the stonfi_v2 liquidity matchers (specs/stonfi_v2_withdraw.mch).

R04 remake of StonfiV2WithdrawLiquidityMatcher, now a FULLY DECLARATIVE build:
both matchers (normal + refund) express their produced data with `out {...}`
build statements, so there is no host builder here — only the opcode / block-type
registration plus the message parsers (`parse pton_* as PTonTransfer`) the
declarative build needs. The shared blocks/liquidity.py build core is still used
by the legacy StonfiV2WithdrawLiquidityMatcher (production path); it is no longer
referenced from the DSL leg.
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
