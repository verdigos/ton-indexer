"""mch host fn + shaper for the base nft_transfer matcher (specs/nft_transfer.mch).

HYBRID declarative build (C++-runnable): the matcher has no `build` directive —
the produced btype, the reject decision and the out-field mapping live in the
.mch; the imperative residue is the single host fn `nft_transfer_data` below. It
delegates to the shared core build_nft_transfer_core in blocks/nft.py (the same
function the legacy NftTransferBlockMatcher's thin wrapper calls): NftTransfer +
optional NftOwnershipAssigned parse, the interface lookups (_get_nft_data plus
the nft_sale / nft_auction purchase detection) and the getgems `is_purchase`
flag that specs/getgems.mch anchors on. `null` from the core (unknown NFT item)
drives `reject when d == null`. The fn returns a namespace with a FIXED key set
(None where the core omits a purchase-only key); out{} maps the always-present
keys plainly and the purchase-only marketplace/price/real_prev_owner keys with
`?:` (omit-when-null), reproducing the legacy variable-key dict. The nested
`nft` dict passes straight through as a field value.

Purchase-parent absorption rides on the shaper, not the fn: the funding parent
is UPSTREAM of the anchor (not a pattern-matched block), so it cannot sit in the
consumed set. The synthesized wrapper merges the consumed set (anchor +
ownership-assigned child + excess), which PLACES the produced block at the
anchor's position; only then is produced.previous_block the funding parent, so
the shaper nft_transfer_parent_absorb runs the parent merge as the last build
step. Legacy did this in one merge over [anchor, parent, children];
here it is two merges (wrapper then shaper), but block topology is compared as a
sorted multiset, so merge order is immaterial to parity.
"""
from __future__ import annotations

from types import SimpleNamespace

from pytoniq_core import Slice

from indexer.events.blocks.core import Block
from indexer.events.blocks.messages import ExcessMessage, NftOwnershipAssigned
from indexer.events.blocks.messages.nft import NftTransfer
from indexer.events.blocks.nft import (
    NftTransferBlock,
    _purchase_parent_to_consume,
    build_nft_transfer_core,
)
from indexer.events.mch.registry import Registries

OPCODES = {
    "NftTransfer":          NftTransfer.opcode,
    "NftOwnershipAssigned": NftOwnershipAssigned.opcode,
    "ExcessMessage":        ExcessMessage.opcode,
}

BLOCK_TYPES = {
    "NftTransferBlock": NftTransferBlock,
}

# Full key set the core may emit. The namespace always carries all of them
# (None where the core omits a purchase-only key); out{} decides present-vs-absent
# per field (plain = always emitted, `?:` = omitted when null).
_NFT_KEYS = (
    "is_purchase", "prev_owner", "new_owner", "query_id", "forward_amount",
    "response_destination", "custom_payload", "forward_payload", "nft",
    "marketplace", "marketplace_address", "price", "real_prev_owner",
)


def register_nft_transfer(reg: Registries) -> Registries:
    """Register everything specs/nft_transfer.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_fn("nft_transfer_data")
    async def nft_transfer_data(transfer, assigned):
        ownership_message = (
            NftOwnershipAssigned(
                Slice.one_from_boc(assigned.event_nodes[0].message.message_content.body))
            if assigned is not None else None
        )
        new_block, _extra = await build_nft_transfer_core(transfer, ownership_message)
        # `_extra` (the funding parent) is absorbed by the shaper post-merge; see
        # the module docstring. None → `reject when d == null` (unknown NFT item).
        if new_block is None:
            return None
        return SimpleNamespace(**{k: new_block.data.get(k) for k in _NFT_KEYS})

    @reg.register_shaper("nft_transfer_parent_absorb")
    def nft_transfer_parent_absorb(produced: Block, match) -> None:
        # Purchase branch only: absorb the funding parent into the produced
        # nft_transfer — the exact block the legacy build appended to its single
        # merge. Runs after the wrapper merge, so produced.previous_block is the
        # funding parent. No-op on plain (non-purchase) transfers.
        if produced.data is None or not produced.data.get("is_purchase"):
            return
        parent = _purchase_parent_to_consume(produced.previous_block)
        if parent is not None:
            # Merging the parent reparents its children (which include the
            # produced block itself) onto produced; compact_connections drops the
            # resulting self-reference, same as the getgems shaper.
            produced.merge_blocks([parent])
            produced.compact_connections()

    return reg


def nft_transfer_registries() -> Registries:
    """Fresh Registries ready to compile specs/nft_transfer.mch."""
    return register_nft_transfer(Registries())
