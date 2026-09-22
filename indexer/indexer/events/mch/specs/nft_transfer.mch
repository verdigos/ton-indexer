# Matches NFT ownership transfers, including GetGems sale and auction purchases.

import "lib/common.mch"
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
    failed when any_failed(transfer)
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
        marketplace_address?: ((account(sale.marketplace_address) ?? addr_none()) if have_sale else (account(auction.mp_addr) ?? addr_none())) if is_purchase else null
        price?:               (amount(sale.full_price) if have_sale else amount(auction.last_bid)) if is_purchase else null
        real_prev_owner?:     real_owner if is_purchase else null
    }
}
