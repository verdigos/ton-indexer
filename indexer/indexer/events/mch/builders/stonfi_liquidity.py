"""mch builders for the stonfi_v2 liquidity matchers (specs/stonfi_v2_withdraw.mch).

R04 remake of StonfiV2WithdrawLiquidityMatcher. Both matchers (normal +
refund) delegate to the shared `build_stonfi_v2_withdraw_core` in
blocks/liquidity.py — the same function the legacy matcher now calls.

The pattern has no recursion, so every capture binds positionally and the
builders are capture-driven (the two pay-to blocks + the burn / refund anchor).
The core re-navigates each pay-to's jetton_transfer child (and the optional
PTonTransfer grandchild) itself, so the builders only forward the pay-to blocks;
the PTonTransfer / jetton_transfer blocks are consumed via the pattern captures
(engine merge), matching the legacy consumed set.
"""
from __future__ import annotations

from indexer.events.blocks.core import Block
from indexer.events.blocks.liquidity import build_stonfi_v2_withdraw_core
from indexer.events.mch.registry import Registries

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

    @reg.register_builder("build_stonfi_v2_withdraw")
    async def build_stonfi_v2_withdraw(m):
        result = await build_stonfi_v2_withdraw_core([m.payout_1, m.payout_2], m.burn, None)
        return result[0] if result is not None else None

    @reg.register_builder("build_stonfi_v2_withdraw_refund")
    async def build_stonfi_v2_withdraw_refund(m):
        result = await build_stonfi_v2_withdraw_core([m.payout_1, m.payout_2], None, m.refund)
        return result[0] if result is not None else None

    return reg


def stonfi_liquidity_registries() -> Registries:
    """Fresh Registries ready to compile specs/stonfi_v2_withdraw.mch."""
    return register_stonfi_liquidity(Registries())
