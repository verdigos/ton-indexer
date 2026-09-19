# Base NFT transfer matcher corresponding to NftTransferBlockMatcher.
#
# This is the base matcher that produces the `nft_transfer` btype consumed
# downstream by specs/getgems.mch (the getgems purchase anchors on it).
# nft_transfer must
# register before getgems so getgems sees the produced block; that ordering is
# wired in event_processing.py.
#
# Matcher shape
# Anchor: the NftTransfer call. Children: the optional NftOwnershipAssigned
# notification (source of `prev_owner`) and the optional excess. The legacy
# child_matcher is an OrMatcher of {NftOwnershipAssigned?, Excess?} that merges
# whichever children match into the nft_transfer. `include_excess` at the root
# only *appends* the anchor's excess to the result list and never merges it,
# so it would not reproduce the legacy topology where the excess is a child of
# the nft_transfer. The excess is therefore matched explicitly in a children set
# (consumed => merged) and `include_excess` is turned off to avoid a duplicate
# standalone append. `include_bounces` is off: the anchor's bounce is not
# absorbed into the nft_transfer, so it stays a standalone call_contract row
# as in legacy.
#
# Purchase-parent consumption (getgems)
# -------------------------------------
# When the interface lookups (nft_sale / nft_auction) confirm a getgems
# purchase, the legacy build sets `is_purchase` and marketplace fields and absorbs
# the funding parent (buyer payment / sale-contract call) into the nft_transfer
# except when the parent is a finish/stop sale-contract ton_transfer. That parent
# is upstream of the anchor, so it cannot ride in the pattern-consumed set. The
# builder returns it; the shaper `nft_transfer_parent_absorb` merges it as the
# last build step, after the wrapper merge has placed the produced block
# at the anchor's position (so produced.previous_block is the funding parent).
# On a plain transfer is_purchase is false and the shaper is a no-op.
#
# Hybrid shell: Fragment URI split has no string-slice builtin, so the nft_item
# metadata projection stays in `nft_transfer_data`. Wire parse, prev_owner, and
# sale/auction purchase detection are declarative.
#
# Capture -> legacy role map:
#   @transfer   the NftTransfer anchor (test_self target)
#   @assigned   the NftOwnershipAssigned notification (prev_owner), when present
#   @funding_parent  the optional unconsumed block immediately before the anchor

import "lib/opcodes.mch"
opcode NftTransfer          = 0x5FCC3D14
matcher nft_transfer {
    produces        nft_transfer
    shape           nft_transfer_parent_absorb
    include_excess  false
    include_bounces false

    op NftTransfer @transfer as NftTransfer {
        -> {
            maybe op NftOwnershipAssigned @assigned as NftOwnershipAssignedPrevOwner
            ; maybe op Excess
        }
        <- maybe peek any @funding_parent
    }

    reject when transfer.body == null
    reject when assigned != null and assigned.body == null
    require nft = nft_transfer_data(account(transfer.msg.transaction.account))
    let new_owner = account(transfer.body.new_owner)
    let prev_owner = (account(assigned.body.prev_owner) ?? addr_none()) if assigned != null else (account(transfer.msg.source) ?? addr_none())
    let rd = account(transfer.body.response_destination)
    let parent_acct = funding_parent.msg.transaction.account
    let owner_match = funding_parent.btype == "ton_transfer" and account(funding_parent.msg.source) == new_owner
    let sale = lookup nft_sale(parent_acct)
    let auction = lookup nft_auction(parent_acct)
    let have_sale = owner_match and sale != null
    let have_auc = not have_sale and auction != null
    let pd_nft = account(sale.nft_address) if have_sale else account(auction.nft_addr)
    let real_owner = (account(sale.nft_owner_address) ?? addr_none()) if have_sale else (account(auction.nft_owner) ?? addr_none())
    let is_purchase = (have_sale or have_auc) and pd_nft == nft.address and real_owner != new_owner
    failed when transfer.failed
    out {
        is_purchase:          is_purchase
        prev_owner:           prev_owner
        new_owner:            new_owner
        query_id:             transfer.body.query_id
        forward_amount:       amount(transfer.body.forward_amount)
        response_destination: rd if rd != addr_none() else null
        custom_payload:       b64(transfer.body.custom_payload)
        forward_payload:      b64(transfer.body.forward_payload)
        nft:                  nft
        marketplace?:         "getgems" if is_purchase else null
        marketplace_address?: ((account(sale.marketplace_address) ?? addr_none()) if have_sale else (account(auction.mp_addr) ?? addr_none())) if is_purchase else null
        price?:               (amount(sale.full_price) if have_sale else amount(auction.last_bid)) if is_purchase else null
        real_prev_owner?:     real_owner if is_purchase else null
    }
}
