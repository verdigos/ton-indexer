# GetGems sale and auction deployments carry listing parameters in message StateInit.
# A sale update can change live state without changing the deployment message.

import "lib/common.mch"
import "lib/getgems.mch"

opcode SaleUpdate          = 0x6C6C2080
opcode NftDiscovery        = 0x2FCB26A2
opcode NftReportStaticData = 0x8B771735
opcode NftSaleCancel       = 0x3
opcode NftAuctionCancel    = 0x5616C572
opcode NftAuctionFinish    = 0x20C9EB18
opcode NftAuctionStop      = 0xB95616B6
opcode TeleitemOutbid      = 0x557CEA20

predicate sale_contract_deploy

fn optional_address(a) = null if a == addr_none() else a

fn mp(a) = account(a) ?? addr_none()

fn finish_auction_result(nft_address, nft_collection, owner, trade_contract) = {
    nft_address:      nft_address
    nft_collection:   nft_collection
    owner:            owner
    trade_contract:   trade_contract
}

fn sale_result(d, v4r1) = null if d == null else {
    full_price:              d.full_price
    marketplace_address:     optional_address(d.marketplace_address)
    marketplace_fee_address: optional_address(d.fees.ref.marketplace_fee_address)
    marketplace_fee:         null if v4r1 else d.fees.ref.marketplace_fee
    royalty_address:         optional_address(d.fees.ref.royalty_address)
    royalty_amount:          null if v4r1 else d.fees.ref.royalty_amount
}

fn sale_data(init, version) = (
    sale_result(parse? init as SaleV1, false)
        if version == 1 else
    sale_result(parse? init as SaleLegacy, false)
        if version == 2 else
    sale_result(parse? init as SaleV4r1, true)
        if version == 4 else
    null
)

fn auction_result(d, inline_fees) = null if d == null else {
    mp_fee_addr:      optional_address(d.fees.ref.mp_fee_addr)
    mp_fee_factor:    d.mp_fee_factor if inline_fees else d.fees.ref.mp_fee_factor
    mp_fee_base:      d.mp_fee_base if inline_fees else d.fees.ref.mp_fee_base
    royalty_fee_addr: optional_address(d.fees.ref.royalty_fee_addr)
    royalty_fee_base: d.royalty_fee_base if inline_fees else d.fees.ref.royalty_fee_base
    max_bid:          d.constant.ref.max_bid
    min_bid:          d.constant.ref.min_bid
}

fn auction_data(init, version) = (
    auction_result(parse? init as AuctionV4r1, false)
        if version == 4 else
    auction_result(parse? init as AuctionV3r2, false)
        if version == 2 else
    auction_result(parse? init as AuctionV1, false)
        if version == 1 else
    auction_result(parse? init as AuctionV3r3, true)
        if version == 3 else
    null
)

matcher nft_update_sale {
    produces nft_update_sale
    include_bounces false

    op SaleUpdate @upd as SaleUpdateMessage

    require sale = getgems_sale(upd.msg.destination)
    failed when tx_failed(upd)
    out {
        sender:              upd.msg.source ?? addr_none()
        sale_contract:       upd.msg.destination ?? addr_none()
        nft_address:         account(sale.nft_address) ?? addr_none()
        new_full_price:      amount(upd.body.new_full_price)
        new_marketplace_fee: amount(upd.body.new_mp_fee)
        new_royalty_amount:  amount(upd.body.new_royalty_amount)
        marketplace_address: mp(sale.marketplace_address)
    }
}

matcher nft_put_on_sale {
    produces        nft_put_on_sale | nft_put_on_auction
    include_excess  false

    btype nft_transfer @listing
      -> { xor {
               btype ton_transfer @sale_init where sale_contract_deploy
               btype call_contract @sale_init where sale_contract_deploy
           }
         ; btype nft_transfer @to_sale }

    reject when account(to_sale.data._nft.address) != account(listing.data._nft.address)
    reject when account(to_sale.data.destination) != sale_init.msg.destination

    let sale    = getgems_sale(sale_init.msg.destination)
    let auction = getgems_auction(sale_init.msg.destination)
    reject when sale == null and auction == null

    reject when sale != null and account(to_sale.data._nft.address) != account(sale.nft_address)
    reject when sale != null and account(listing.data.source) != account(sale.nft_owner_address)
    reject when sale == null and account(to_sale.data._nft.address) != account(auction.nft_addr)
    reject when sale == null and account(listing.data.source) != account(auction.nft_owner)

    let init = state_init(sale_init)
    let sd = sale_data(init, getgems_sale_version(sale.code_hash))
    let ad = auction_data(init, getgems_auction_version(auction.code_hash))
    reject when sale != null and sd == null
    reject when sale == null and ad == null

    produces switch {
        when sale != null => nft_put_on_sale out {
            nft_address:             account(to_sale.data._nft.address) ?? addr_none()
            nft_index:               to_sale.data._nft.index
            nft_collection:          account(to_sale.data._nft.collection.address) ?? addr_none()
            owner:                   account(sale.nft_owner_address) ?? addr_none()
            listing_address:         account(listing.data.destination) ?? addr_none()
            sale_address:            sale_init.msg.destination ?? addr_none()
            full_price:              amount(sd.full_price)
            marketplace_address:     mp(sd.marketplace_address)
            marketplace_fee_address: account(sd.marketplace_fee_address) ?? addr_none()
            marketplace_fee:         amount(sd.marketplace_fee)
            royalty_address:         account(sd.royalty_address) ?? addr_none()
            royalty_amount:          amount(sd.royalty_amount)
        }
        else => nft_put_on_auction out {
            nft_address:         account(to_sale.data._nft.address) ?? addr_none()
            nft_index:           to_sale.data._nft.index
            nft_collection:      account(to_sale.data._nft.collection.address) ?? addr_none()
            owner:               account(auction.nft_owner) ?? addr_none()
            listing_address:     account(listing.data.destination) ?? addr_none()
            auction_address:     sale_init.msg.destination ?? addr_none()
            marketplace_address: mp(auction.mp_addr)
            mp_fee_address:      account(ad.mp_fee_addr)
            mp_fee_factor:       amount(ad.mp_fee_factor)
            mp_fee_base:         amount(ad.mp_fee_base)
            royalty_fee_addr:    account(ad.royalty_fee_addr)
            royalty_fee_base:    amount(ad.royalty_fee_base)
            max_bid:             amount(ad.max_bid)
            min_bid:             amount(ad.min_bid)
        }
    }
}

matcher nft_discovery {
    produces nft_discovery

    op NftDiscovery @disc -> op NftReportStaticData @report as NftReportStaticData

    out {
        sender:            disc.msg.source ?? addr_none()
        nft:               disc.msg.destination ?? addr_none()
        result_collection: account(report.body.collection) ?? addr_none()
        result_index:      report.body.index
        query_id:          report.body.query_id
    }
}

matcher getgems_auction_bid {
    produces        auction_bid
    include_excess  false

    xor {
        btype ton_transfer @bid
        btype call_contract @bid
    }

    let source = bid.msg.source
    reject when source == null

    let auction_addr = bid.msg.destination
    require auction = lookup nft_auction(auction_addr)

    let raw_op = bid.msg.opcode
    let body_op = ((raw_op + 4294967296) if raw_op < 0 else raw_op) if raw_op != null else null
    let comment = bid.data.comment if bid.btype == "ton_transfer" else null
    let cancel_text = comment in ["cancel", "finish", "stop"]
    let version = getgems_auction_version(auction.code_hash)
    let v1_bidder = source != account(auction.nft_owner) and source != account(auction.mp_addr)
    reject when not (
        source != account(auction.nft_addr) and
        ((version == 1 and v1_bidder) or (version == 2 and not cancel_text) or
         (version == 3 and not cancel_text and not (body_op in [1, 2, 555])) or
         (version == 4 and (body_op == null or body_op == 0 or body_op == 0x344EF025 or body_op == 0x9AF07447)))
    )

    let nft_addr = auction.nft_addr
    # NFT item lookup miss keeps the bid row with null item index and collection.
    let item = lookup nft_item(nft_addr)

    failed when bid.failed
    out {
        amount:         amount(bid.msg.value)
        bidder:         source
        auction:        account(auction_addr)
        nft_address:    account(nft_addr)
        nft_item_index: item.index if item != null else null
        nft_collection: account(item.collection_address) if item != null else null
    }
}

matcher auction_bid {
    produces        auction_bid
    include_excess  false

    xor {
        btype ton_transfer @bid
        btype call_contract @bid
    }

    let source = bid.msg.source
    let raw_op = bid.msg.opcode
    let body_op = ((raw_op + 4294967296) if raw_op < 0 else raw_op) if raw_op != null else null
    reject when source == null or not (body_op == null or body_op == 0)

    let nft_addr = bid.msg.destination
    # NFT item lookup miss keeps a Fragment bid row with null item index and collection.
    let item = lookup nft_item(nft_addr)
    let dns = item != null and (item.code_hash == "i1/8nr/TkGTY1fVuRlnIJrt1k5I/XKSHKL5NYK9vUfk=" or
                                item.collection_address == "0:B774D95EB20543F186C06B371AB88AD704F7E256130CAF96189368A7D0CB6CCF")
    let fragment = is_teleitem(nft_addr)
    reject when not (dns or fragment)
    reject when lookup nft_auction(nft_addr) != null

    failed when bid.failed
    out {
        amount:         amount(bid.msg.value)
        bidder:         source
        auction:        account(nft_addr)
        nft_address:    account(nft_addr)
        nft_item_index: item.index if item != null else null
        nft_collection: account(item.collection_address) if item != null else null
    }
}

matcher auction_outbid {
    produces auction_outbid

    xor {
        btype ton_transfer @leg
        op TeleitemOutbid @leg
    }
      <- peek btype auction_bid @bid

    let getgems = leg.btype == "ton_transfer"
    reject when account(leg.data.source) != account(bid.data.auction)
    reject when getgems and not contains(leg.data.comment ?? "", "Your bid has been outbid by another user")
    out {
        auction_address: bid.data.auction
        nft:             bid.data.nft_address
        nft_collection:  bid.data.nft_collection
        bidder:          leg.msg.destination ?? addr_none()
        new_bidder:      bid.data.bidder
        amount:          amount(leg.msg.value)
        comment:         leg.data.comment if getgems else null
    }
}

matcher nft_cancel_sale {
    produces nft_cancel_sale

    op NftSaleCancel @cancel
      -> peek btype nft_transfer @ret

    require sale = getgems_sale(cancel.msg.destination)
    reject when account(ret.data._nft.address) != account(sale.nft_address)
    failed when tx_failed(cancel)
    out {
        nft_address:         ret.data._nft.address
        nft_collection:      ret.data._nft.collection.address
        owner:               ret.data.destination
        trade_contract:      cancel.msg.destination ?? addr_none()
        marketplace_address: mp(sale.marketplace_address)
    }
}

matcher nft_finish_auction {
    produces nft_finish_auction

    xor {
        btype ton_transfer @cancel
        op NftAuctionStop @cancel
        op NftAuctionFinish @cancel
    }
      -> peek btype nft_purchase @ret

    reject when cancel.btype == "ton_transfer" and not (cancel.data.comment in ["finish", "stop"])
    require auction = getgems_auction(cancel.msg.destination)
    reject when account(ret.data.nft_address) != account(auction.nft_addr)
    failed when tx_failed(cancel)
    let finish = finish_auction_result(ret.data.nft_address, ret.data.collection_address, ret.data.new_owner, cancel.msg.destination ?? addr_none())
    out {
        nft_address:         finish.nft_address
        nft_collection:      finish.nft_collection
        owner:               finish.owner
        trade_contract:      finish.trade_contract
        marketplace_address: mp(auction.mp_addr)
    }
}

matcher nft_cancel_auction {
    produces nft_cancel_auction | nft_finish_auction

    xor {
        btype ton_transfer @cancel
        op NftAuctionCancel @cancel
        op NftAuctionFinish @cancel
        op NftAuctionStop @cancel
    }
      -> peek btype nft_transfer @ret

    reject when cancel.btype == "ton_transfer" and not (cancel.data.comment in ["cancel", "finish", "stop"])
    require auction = getgems_auction(cancel.msg.destination)
    reject when account(ret.data._nft.address) != account(auction.nft_addr)
    failed when tx_failed(cancel)
    let finish = finish_auction_result(ret.data._nft.address, ret.data._nft.collection.address, ret.data.destination, cancel.msg.destination ?? addr_none())

    produces switch {
        when (cancel.btype == "ton_transfer" and (cancel.data.comment in ["finish", "stop"])) or (cancel.btype == "call_contract" and (cancel.data.opcode in [0xB95616B6, 0x20C9EB18])) => nft_finish_auction out {
            nft_address:         finish.nft_address
            nft_collection:      finish.nft_collection
            owner:               finish.owner
            trade_contract:      finish.trade_contract
            marketplace_address: mp(auction.mp_addr)
        }
        else => nft_cancel_auction out {
            nft_address:         finish.nft_address
            nft_collection:      finish.nft_collection
            owner:               finish.owner
            trade_contract:      finish.trade_contract
            marketplace_address: mp(auction.mp_addr)
        }
    }
}
