"""mch builder for the telegram_nft_purchase matcher (specs/telegram_nft.mch).

Thin adapter over the shared core `build_telegram_nft_purchase_core` in
blocks/nft.py — the same function the legacy TelegramNftPurchaseBlockMatcher
calls. Capture-driven: the pattern has no recursion and no same-head XOR, so
@assigned / @payment / @payout_1 / @payout_2 bind positionally on both the
compiled and the IR paths. The core recomputes every data field and the
consumed set from those blocks, so the produced block is byte-identical to the
legacy matcher's.
"""
from __future__ import annotations

from indexer.events.blocks.messages.nft import AuctionFillUp, NftOwnershipAssigned
from indexer.events.blocks.nft import (
    NftTransferBlock,
    build_telegram_nft_purchase_core,
)
from indexer.events.mch.registry import Registries

OPCODES = {
    "NftOwnershipAssigned": NftOwnershipAssigned.opcode,
    "AuctionFillUp":        AuctionFillUp.opcode,
}

BLOCK_TYPES = {
    "NftTransferBlock": NftTransferBlock,
}


def register_telegram_nft(reg: Registries) -> Registries:
    """Register everything specs/telegram_nft.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_builder("build_telegram_nft_purchase")
    async def build_telegram_nft_purchase(m):
        payouts = [p for p in (m.payout_1, m.payout_2) if p is not None]
        new_block, _extra = await build_telegram_nft_purchase_core(m.assigned, m.payment, payouts)
        # The synthesized wrapper merges [anchor] + consumed set (payment +
        # payouts), which equals the legacy [anchor] + extra set. Return the
        # unmerged block, or None to reject (spec §10.7).
        return new_block

    return reg


def telegram_nft_registries() -> Registries:
    """Fresh Registries ready to compile specs/telegram_nft.mch."""
    return register_telegram_nft(Registries())
