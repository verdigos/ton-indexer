# Telegram (Fragment) NFT purchase matcher corresponding to
# TelegramNftPurchaseBlockMatcher.
#
# The legacy matcher is a workaround: it anchors on the NftOwnershipAssigned
# call, then imperatively walks `previous_block` and scans that parent's
# next_blocks for AuctionFillUp payouts inside build_block (no declarative
# structure). The internal walk may differ as long as the output is identical;
# here the whole shape is expressed declaratively:
#
#   the funding payment (a ton_transfer to the marketplace, or the external
#   message that drove the NFT item directly) is the anchor's PARENT; all
#   AuctionFillUp payouts are captured as a list of that parent's children.
#   In the `A <- B -> {many C}` shape, the parent edge is
#   anchor-headed, and children after a parent edge attach to the parent matcher
#   rather than to the anchor.
#
# The AuctionFillUp children set is what makes the parent edge fire only on
# real fragment purchases (an ordinary orphan NftOwnershipAssigned has no such
# siblings), so no data predicate is needed to gate the parent consumption.
# The host keeps the soft ownership+payload parse and metadata projection.
# Payout sort/select is `stable_sort_by` then `at`. Parent-kind gate, is_mint,
# purchase assembly, and the 16-key record are declarative.

# Two matchers, split on the legacy CONSUMED SET
# ----------------------------------------------
# Legacy `build_block` always produces a block, but only absorbs the parent and
# the payouts inside one branch — `payload is TeleitemBidInfo` AND the parent is
# a ton_transfer or an EXTERNAL-driven call (`get_message().source is None`).
# Every other assignment (telemint mint parent, orphan assignment) merges the
# ANCHOR ALONE and leaves the parent standing, which is what keeps the separate
# `nft_mint` action alive next to the `nft_transfer` one. A single mch pattern
# cannot express both consumed sets, so the split is by matcher:
#
#   telegram_nft_purchase   the consuming branch. Parent XOR-alternative over
#                           the two legacy parent kinds, payouts attached inside
#                           each branch (a children block hung off the `or` node
#                           itself is silently dropped by the engines).
#   telegram_nft_assigned   the non-consuming branch: anchor alone, with the
#                           optional parent and its AuctionFillUp children
#                           peeked; only purchase rows publish the payout and
#                           royalty fields. Runs after the purchase matcher.
#
# The second matcher peeks its optional parent so the shared core can detect a
# telemint mint without adding that parent to the consumed set.
#
# Residual gap (documented, not in the corpus): a teleitem-bid assignment with a
# ton_transfer/external parent and ZERO AuctionFillUp payouts. Legacy absorbs the
# parent; here the purchase matcher misses (at least one payout is required, the
# gate that keeps it off ordinary transfers) and the assigned matcher produces
# the same row without absorbing the parent. Row fields are identical; only the
# leftover parent block differs.
#
# Capture -> legacy role map:
#   @assigned   the NftOwnershipAssigned anchor
#   @payment    block.previous_block (the funding ton_transfer, or the external
#               call that drove the item directly — Fragment signer shape)
#   @payouts    all payouts, sorted by created_lt. In the purchase matcher (at
#               most two): one is the seller, two are royalty then seller. In
#               the assigned matcher: one is the seller, two are royalty then
#               seller, three or more publish no payout or royalty field.

import "lib/fragment.mch"
import "lib/opcodes.mch"

fn is_telemint_mint(p) = p != null and ((p.btype == "call_contract" and p.data.opcode == 0x299a3e15) or p.btype == "nft_mint")

matcher telegram_nft_purchase {
    produces nft_transfer

    # The two legacy parent kinds, XOR because `ton_transfer` and `call_contract`
    # are disjoint btypes. The payout children hang off EACH branch, not off the
    # alternative: `_match_or` walks branches only, so children recorded on the
    # `or` node itself would never match and never consume.
    # The item emits at most royalty + beneficiary.
    op NftOwnershipAssigned @assigned
      <- xor {
             btype ton_transfer @payment
               -> { many[1..2] op AuctionFillUp @payouts }
             btype call_contract @payment where (.source == null)
               -> { many[1..2] op AuctionFillUp @payouts }
         }

    # Parent-kind gate is the xor above. Royalty fields need two or more payouts.
    require d = telegram_nft_purchase_data(assigned)
    let is_purchase = d.is_bid and not is_telemint_mint(payment)
    # Venue label: the collection's stored minter key decides it. An unknown key
    # (no collection address, or a lookup miss) keeps "fragment"; a known
    # foreign key drops the label.
    let coll = lookup telemint_collection(d.nft.collection.address) if d.nft.collection != null else null
    let sorted = stable_sort_by(payouts as p => p.msg.created_lt)
    let n = len(sorted)
    let royalty = at(sorted, 0) if d.is_bid and n > 1 else null
    let seller = at(sorted, 1) if d.is_bid and n > 1 else (at(sorted, 0) if d.is_bid else null)
    out {
        is_purchase:          is_purchase
        prev_owner:           d.prev_owner
        new_owner:            d.new_owner
        query_id:             d.query_id
        forward_amount:       null
        response_destination: null
        custom_payload:       null
        forward_payload:      b64(d.payload)
        nft:                  d.nft
        real_prev_owner:      null
        price:                amount(d.bid) if d.is_bid else null
        marketplace:          fragment_marketplace(coll) if d.is_bid else null
        payout_amount:        get(seller.data, "value")
        payout_address:       get(seller.data, "destination")
        royalty_amount?:      get(royalty.data, "value")
        royalty_address?:     get(royalty.data, "destination")
    }
}

matcher telegram_nft_assigned {
    produces nft_transfer

    # The optional parent is observed without changing the anchor-only consumed set.
    op NftOwnershipAssigned @assigned
      <- maybe peek any @payment -> { peek many[0..] op AuctionFillUp @payouts }

    require d = telegram_nft_purchase_data(assigned)
    # An owner's NftTransfer command forwards a caller-chosen payload, so it must not decide the price or the venue.
    let is_bid = d.is_bid and not (
        payment != null and payment.btype == "call_contract"
        and payment.data.opcode == 0x5FCC3D14
        and payment.data.source != null and payment.data.source == d.prev_owner)
    let is_purchase = is_bid and not is_telemint_mint(payment)
    # Same venue gate as the purchase matcher; the else branch below carries no
    # marketplace key at all.
    let coll = lookup telemint_collection(d.nft.collection.address) if d.nft.collection != null else null
    let sorted = stable_sort_by(payouts as p => p.msg.created_lt)
    let n = len(sorted)
    let royalty = at(sorted, 0) if is_purchase and n == 2 else null
    let seller = at(sorted, 1) if is_purchase and n == 2 else (at(sorted, 0) if is_purchase and n == 1 else null)

    # A `produces switch`, not `?:` keys, because the legacy key set here is
    # BRANCH-shaped, not null-shaped. `payload is TeleitemBidInfo` sets all three
    # of price / marketplace / real_prev_owner — and sets real_prev_owner to None
    # explicitly, a PRESENT key holding null. `?:` is omit-when-null, so it can
    # emit either the present-null key or nothing, never one per branch. The
    # switch condition is `is_bid`, that branch minus an owner-forwarded spoof.
    # Payouts come from free fill-up siblings and emit only when is_purchase; a peeked fill-up stays a separate call_contract action.
    produces switch {
        when is_bid => nft_transfer out {
            is_purchase:          is_purchase
            prev_owner:           d.prev_owner
            new_owner:            d.new_owner
            query_id:             d.query_id
            forward_amount:       null
            response_destination: null
            custom_payload:       null
            forward_payload:      b64(d.payload)
            nft:                  d.nft
            real_prev_owner:      null
            price:                amount(d.bid)
            marketplace:          fragment_marketplace(coll)
            payout_amount?:       get(seller.data, "value")
            payout_address?:      get(seller.data, "destination")
            royalty_amount?:      get(royalty.data, "value")
            royalty_address?:     get(royalty.data, "destination")
        }
        else => nft_transfer out {
            is_purchase:          is_purchase
            prev_owner:           d.prev_owner
            new_owner:            d.new_owner
            query_id:             d.query_id
            forward_amount:       null
            response_destination: null
            custom_payload:       null
            forward_payload:      b64(d.payload)
            nft:                  d.nft
        }
    }
}
