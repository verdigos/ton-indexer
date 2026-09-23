# Telegram NFT purchase and ownership assignment flows classify teleitem transfers and payouts.

import "lib/opcodes.mch"

fn is_telemint_mint(p) = p != null and ((p.btype == "call_contract" and p.data.opcode == 0x299a3e15) or p.btype == "nft_mint")

matcher telegram_nft_purchase {
    produces nft_transfer

    op NftOwnershipAssigned @assigned
      <- xor {
             btype ton_transfer @payment
               -> { many[1..2] op AuctionFillUp @payouts }
             btype call_contract @payment where (.source == null)
               -> { many[1..2] op AuctionFillUp @payouts }
         }

    require d = telegram_nft_purchase_data(assigned)
    let is_purchase = d.is_bid and not is_telemint_mint(payment)
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
        payout_amount:        get(seller.data, "value")
        payout_address:       get(seller.data, "destination")
        royalty_amount?:      get(royalty.data, "value")
        royalty_address?:     get(royalty.data, "destination")
    }
}

matcher telegram_nft_assigned {
    produces nft_transfer

    op NftOwnershipAssigned @assigned
      <- maybe peek any @payment -> { peek many[0..] op AuctionFillUp @payouts }

    require d = telegram_nft_purchase_data(assigned)
    let is_bid = d.is_bid and not (
        payment != null and payment.btype == "call_contract"
        and payment.data.opcode == 0x5FCC3D14
        and payment.data.source != null and payment.data.source == d.prev_owner)
    let is_purchase = is_bid and not is_telemint_mint(payment)
    let sorted = stable_sort_by(payouts as p => p.msg.created_lt)
    let n = len(sorted)
    let royalty = at(sorted, 0) if is_purchase and n == 2 else null
    let seller = at(sorted, 1) if is_purchase and n == 2 else (at(sorted, 0) if is_purchase and n == 1 else null)

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
