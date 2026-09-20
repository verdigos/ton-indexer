# Matches Fragment teleitem auction starts and cancellations.

import "lib/common.mch"

opcode TeleitemStartAuction  = 0x487A8E81
opcode TeleitemCancelAuction = 0x371638AE
opcode TeleitemOk            = 0xA37A0983

matcher teleitem_start_auction {
    produces        teleitem_start_auction
    include_excess  false
    include_bounces false

    op TeleitemStartAuction @auction as TeleitemStartAuction -> maybe op TeleitemOk

    require nft = lookup nft_item(auction.msg.destination)
    reject when not is_teleitem(auction.msg.destination)
    let item  = auction.msg.destination ?? addr_none()
    let owner = auction.msg.source ?? addr_none()
    failed when tx_failed(auction)
    out {
        nft_address:         item
        nft_index:           nft.index
        nft_collection:      account(nft.collection_address)
        owner:               owner
        listing_address:     null
        auction_address:     item
        marketplace_address: null
        marketplace:         "fragment"
        mp_fee_address:      account(auction.body.config.ref.beneficiary_address)
        mp_fee_factor:       amount(0)
        mp_fee_base:         amount(0)
        royalty_fee_addr:    null
        royalty_fee_base:    amount(0)
        max_bid:             amount(auction.body.config.ref.max_bid)
        min_bid:             amount(auction.body.config.ref.initial_min_bid)
    }
}

matcher teleitem_cancel_auction {
    produces        teleitem_cancel_auction
    include_excess  false
    include_bounces false

    op TeleitemCancelAuction @cancel -> maybe op TeleitemOk

    require nft = lookup nft_item(cancel.msg.destination)
    reject when not is_teleitem(cancel.msg.destination)
    let item = cancel.msg.destination ?? addr_none()
    failed when tx_failed(cancel)
    out {
        nft_address:         item
        nft_collection:      account(nft.collection_address) ?? addr_none()
        owner:               cancel.msg.source ?? addr_none()
        trade_contract:      item
        marketplace_address: null
    }
}
