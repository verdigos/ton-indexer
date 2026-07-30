"""mch host fn for the telegram_nft_purchase matcher (specs/telegram_nft.mch).

HYBRID declarative build (C++-runnable): the matcher has no `build` directive —
the produced btype, the reject decision and the out-field mapping live in the
.mch; the imperative residue is the single host fn `telegram_nft_purchase_data`
below. It delegates to the shared core `build_telegram_nft_purchase_core` in
blocks/nft.py (the same function the legacy matcher calls) and exposes the
produced data dict as a namespace with a FIXED key set (`.get(k)` → None for the
keys the core omits in a given branch). The out{} maps the always-present keys
plainly and the payout-count-conditional royalty keys with `?:` (omit-when-null),
reproducing the legacy variable-key dict. `null` from the core → `reject when`.

Consumed set: the pattern captures @payment + @payout_1/@payout_2, which the
synthesized wrapper merges — identical to the legacy [anchor] + extra set — so
no shaper is needed.
"""
from __future__ import annotations

from types import SimpleNamespace

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

# Full key set the core may emit. The namespace always carries all of them
# (None where the core's branch omits the key); out{} decides present-vs-absent
# per field (plain = always emitted, `?:` = omitted when null).
_TELEGRAM_KEYS = (
    "is_purchase", "new_owner", "prev_owner", "query_id", "forward_amount",
    "response_destination", "custom_payload", "forward_payload", "nft",
    "real_prev_owner", "price", "marketplace",
    "royalty_amount", "royalty_address", "payout_amount", "payout_address",
)


def register_telegram_nft(reg: Registries) -> Registries:
    """Register everything specs/telegram_nft.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_fn("telegram_nft_purchase_data")
    async def telegram_nft_purchase_data(assigned, payment, payout_1, payout_2):
        payouts = [p for p in (payout_1, payout_2) if p is not None]
        # telegram_nft_assigned passes @payment unbound: its pattern deliberately
        # has no parent edge (matching one would consume it), but the core still
        # needs the parent to decide is_mint. Read it where legacy reads it.
        prev = payment if payment is not None else assigned.previous_block
        new_block, _extra = await build_telegram_nft_purchase_core(assigned, prev, payouts)
        if new_block is None:
            return None
        data = new_block.data
        return SimpleNamespace(**{k: data.get(k) for k in _TELEGRAM_KEYS})

    return reg


def telegram_nft_registries() -> Registries:
    """Fresh Registries ready to compile specs/telegram_nft.mch."""
    return register_telegram_nft(Registries())
