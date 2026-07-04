"""mch builder for the tonstakers withdraw matcher (specs/tonstakers.mch).

Thin adapter: extracts the captured request + immediate/delayed withdrawal
blocks from the runtime Match and delegates to the shared build core in
blocks/staking.py — the same function the legacy TONStakersWithdrawMatcher
calls. The core returns either a TONStakersWithdrawBlock (immediate arm) or a
TONStakersWithdrawRequestBlock (delayed arm) — the produces-union of
specs/tonstakers.mch — and the synthesized wrapper merges the consumed blocks.

The captures replace the legacy get_labeled scans: @request, @immediate_withdrawal
and @delayed_withdrawal bind the same blocks the labels did (immediate/delayed sit
on the two exclusive fork branches, so exactly one is non-None per match). The
delayed arm's minted NFT is still read off delayed_withdrawal.next_blocks inside
the core — capturing it as a pattern node would consume it and diverge from the
legacy consumed set (it stays a separate nft_mint action).
"""
from __future__ import annotations

from indexer.events.blocks.messages.jettons import (
    JettonBurn,
    JettonBurnNotification,
)
from indexer.events.blocks.messages.staking import (
    TONStakersMintNFT,
    TONStakersPoolWithdrawal,
    TONStakersWithdrawRequest,
)
from indexer.events.blocks.staking import (
    TONStakersWithdrawBlock,
    TONStakersWithdrawRequestBlock,
    build_tonstakers_withdraw_core,
)
from indexer.events.mch.registry import Registries

OPCODES = {
    "JettonBurn":                JettonBurn.opcode,
    "JettonBurnNotification":    JettonBurnNotification.opcode,
    "TONStakersWithdrawRequest": TONStakersWithdrawRequest.opcode,
    "TONStakersPoolWithdrawal":  TONStakersPoolWithdrawal.opcode,
    "TONStakersMintNFT":         TONStakersMintNFT.opcode,
}

BLOCK_TYPES = {
    "TONStakersWithdrawBlock":        TONStakersWithdrawBlock,
    "TONStakersWithdrawRequestBlock": TONStakersWithdrawRequestBlock,
}


def register_tonstakers(reg: Registries) -> Registries:
    """Register everything specs/tonstakers.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_builder("build_tonstakers_withdraw")
    async def build_tonstakers_withdraw(m):
        return build_tonstakers_withdraw_core(
            m.burn, m.request, m.immediate_withdrawal, m.delayed_withdrawal
        )

    return reg


def tonstakers_registries() -> Registries:
    """Fresh Registries ready to compile specs/tonstakers.mch."""
    return register_tonstakers(Registries())
