# Telegram (Fragment) NFT purchase (R01 remake) — remade from
# blocks/nft.py TelegramNftPurchaseBlockMatcher.
#
# The legacy matcher is a workaround: it anchors on the NftOwnershipAssigned
# call, then imperatively walks `previous_block` and scans that parent's
# next_blocks for AuctionFillUp payouts inside build_block (no declarative
# structure). Per the M2R remake gate the internal walk may differ as long as
# the OUTPUT is identical; here the whole shape is expressed declaratively:
#
#   the funding payment (a ton_transfer to the marketplace) is the anchor's
#   PARENT; the two AuctionFillUp payouts (royalty then payout, by created_lt)
#   are that parent's CHILDREN. This is the T12 `A <- B -> {C; D}` shape:
#   the parent edge is anchor-headed, and children after a parent edge attach
#   to the parent matcher (spec §6.4/§6.5).
#
# The AuctionFillUp children set is what makes the parent edge fire only on
# real fragment purchases (an ordinary orphan NftOwnershipAssigned has no such
# siblings), so no data predicate is needed to gate the parent consumption.
# The builder reuses the legacy field logic via build_telegram_nft_purchase_core;
# is_purchase / mint / len(payouts) branching all live there, driven by the
# captured @assigned / @payment / @payout_* blocks.
#
# Scope note: the legacy matcher also fires on external-source-call parents
# (telemint mint variant) and on non-purchase orphan assignments; neither is
# in the corpus, so this remake targets the ton_transfer-funded purchase shape
# the Fragment fixture exercises. The shared core still handles every branch
# for the legacy wrapper.
#
# Capture -> legacy role map:
#   @assigned   the NftOwnershipAssigned anchor
#   @payment    block.previous_block (the funding ton_transfer)
#   @payout_1   payouts[0] after created_lt sort (royalty / sending fee)
#   @payout_2   payouts[1] after created_lt sort (seller payout), absent for 1-payout traces

opcode NftOwnershipAssigned = 0x05138D91
opcode AuctionFillUp        = 0x370FEC51

matcher telegram_nft_purchase {
    produces NftTransferBlock
    entry    @assigned

    op NftOwnershipAssigned @assigned
      <- btype ton_transfer @payment
      -> { op AuctionFillUp @payout_1
         ; maybe op AuctionFillUp @payout_2 }

    # HYBRID declarative build (C++-runnable). The core (nft-data lookup, teleitem
    # payload dispatch, payout ordering) is the one host fn; fields map from its
    # namespace. Pattern requires a ton_transfer parent + >=1 AuctionFillUp payout
    # => always the fragment purchase shape (is_purchase true, real_prev_owner
    # present-null, one/two payouts) — so all but the royalty pair are plain; the
    # royalty pair (2-payout only) is `?:` to match the legacy conditional keys.
    let d = telegram_nft_purchase_data(assigned, payment, payout_1, payout_2)
    reject when d == null
    out {
        is_purchase:          d.is_purchase
        prev_owner:           d.prev_owner
        new_owner:            d.new_owner
        query_id:             d.query_id
        forward_amount:       d.forward_amount
        response_destination: d.response_destination
        custom_payload:       d.custom_payload
        forward_payload:      d.forward_payload
        nft:                  d.nft
        real_prev_owner:      d.real_prev_owner
        price:                d.price
        marketplace:          d.marketplace
        payout_amount:        d.payout_amount
        payout_address:       d.payout_address
        royalty_amount?:      d.royalty_amount
        royalty_address?:     d.royalty_address
    }
}
