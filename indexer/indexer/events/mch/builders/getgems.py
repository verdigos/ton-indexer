"""mch host fn + shaper for the getgems_nft_purchase matcher (specs/getgems.mch).

HYBRID declarative build (C++-runnable): the matcher has no `build` directive —
the produced btype, the reject decision and the out-field mapping live in the
.mch, and the imperative residue is the single host fn `getgems_nft_purchase_data`
below. It delegates to the shared core `build_getgems_nft_purchase_core` in
blocks/nft.py (the same function the legacy matcher's thin wrapper calls) and
exposes the produced NftPurchaseData dataclass as a namespace the out{} reads.
`null` from the core (no real_prev_owner / seller payout not found) drives
`reject when d == null`.

Two host predicates reproduce the legacy `test_self` (getgems_purchase) and the
legacy seller-payout scan condition (getgems_seller_payout, restricted to the
normal branch so the finish/stop branch always flows through the shaper, exactly
as legacy switches on `previous_block` being a finish/stop ton_transfer).

The shaper `getgems_proxy_insert` carries the finish/stop branch: verbatim
EmptyBlock + `insert_between`, plus absorbing the sibling payout (which the
pattern could not consume) at the same point legacy's single merge did.
"""
from __future__ import annotations

from dataclasses import fields as dc_fields
from types import SimpleNamespace

from indexer.events.blocks.basic_blocks import TonTransferBlock
from indexer.events.blocks.core import Block, EmptyBlock
from indexer.events.blocks.nft import (
    NftPurchaseBlock,
    build_getgems_nft_purchase_core,
)
from indexer.events.mch.registry import Registries

BLOCK_TYPES = {
    "NftPurchaseBlock": NftPurchaseBlock,
}

_FINISH_STOP = ("finish", "stop")


def _from_finish_stop_sale(nft_block: Block) -> bool:
    """The legacy `need_proxy` condition: the anchor's parent is a finish/stop
    ton_transfer (the sale contract finalizing the deal)."""
    prev = nft_block.previous_block
    return isinstance(prev, TonTransferBlock) and prev.comment in _FINISH_STOP


def _find_seller_payout(nft_block: Block) -> Block | None:
    """Verbatim legacy candidate scan for the seller ton_transfer: among the
    finish/stop parent's children (siblings) in the sale-contract branch, else
    among the nft_transfer's own children."""
    real_prev_owner = nft_block.data.get("real_prev_owner")
    if real_prev_owner is None:
        return None
    candidates = nft_block.next_blocks
    if _from_finish_stop_sale(nft_block):
        candidates = nft_block.previous_block.next_blocks
    for n in candidates:
        if n is nft_block:
            continue
        if n.btype == "ton_transfer" and n.get_message().destination == real_prev_owner:
            return n
    return None


def register_getgems(reg: Registries) -> Registries:
    """Register everything specs/getgems.mch needs into `reg`."""
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_predicate("getgems_purchase")
    def getgems_purchase(block: Block) -> bool:
        return (
            block.btype == "nft_transfer"
            and block.data is not None
            and block.data.get("is_purchase") is True
            and block.data.get("marketplace") == "getgems"
        )

    @reg.register_predicate("getgems_seller_payout")
    def getgems_seller_payout(block: Block) -> bool:
        # Normal branch only: capture the seller payout as a CHILD of the anchor.
        # In the finish/stop branch legacy scans siblings, not children, so this
        # predicate must stay false there and let the shaper handle the payout.
        anchor = block.previous_block
        return (
            block.btype == "ton_transfer"
            and anchor is not None
            and anchor.data is not None
            and not _from_finish_stop_sale(anchor)
            and block.get_message().destination == anchor.data.get("real_prev_owner")
        )

    @reg.register_fn("getgems_nft_purchase_data")
    async def getgems_nft_purchase_data(nft):
        # Imperative residue: the seller-payout scan (normal child branch AND the
        # finish/stop sibling branch) + the core field build. Data (incl. payout
        # fields) comes from the seller ton_transfer. Returns a namespace mirroring
        # NftPurchaseData for the out{} mapping, or None → `reject when d == null`.
        # The consumed set (anchor + captured @payout in the normal branch, plus
        # the sibling payout the shaper adds in the finish/stop branch) is the
        # engine's / shaper's job, not this fn's.
        payout = _find_seller_payout(nft)
        block = build_getgems_nft_purchase_core(nft, payout)
        if block is None:
            return None
        return SimpleNamespace(
            **{f.name: getattr(block.data, f.name) for f in dc_fields(block.data)}
        )

    @reg.register_shaper("getgems_proxy_insert")
    def getgems_proxy_insert(produced: Block, match) -> None:
        # Finish/stop sale-contract branch only. Verbatim legacy surgery: insert
        # an EmptyBlock proxy between the (unconsumed) finish/stop parent and
        # [produced, payout]. Because the sibling payout was NOT pattern-consumed
        # (the parent is unconsumed, so no tree edge reaches the sibling), the
        # shaper also merges it into the produced block here — the same absorption
        # legacy did via its single merge over [nft, payout, proxy].
        nft = match.nft
        parent = produced.previous_block  # the finish/stop parent (post-merge)
        if not (isinstance(parent, TonTransferBlock) and parent.comment in _FINISH_STOP):
            return  # normal branch: no proxy, no surgery
        real_prev_owner = nft.data.get("real_prev_owner")
        payout = None
        for n in parent.next_blocks:
            if n is produced:
                continue
            if n.btype == "ton_transfer" and n.get_message().destination == real_prev_owner:
                payout = n
                break
        if payout is None:
            return
        proxy = EmptyBlock()
        parent.insert_between([produced, payout], proxy)
        produced.merge_blocks([payout, proxy])
        produced.compact_connections()

    return reg


def getgems_registries() -> Registries:
    """Fresh Registries ready to compile specs/getgems.mch."""
    return register_getgems(Registries())
