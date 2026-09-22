# Matches TON DNS purchases, record changes, renewals, deletions, and balance releases.
import "lib/common.mch"
import "lib/opcodes.mch"

matcher dns_purchase {
    produces dns_purchase

    op AuctionFillUp @anchor as AuctionFillUpMessage
      <- peek btype call_contract @purchase_parent

    let buyer = purchase_parent.msg.source ?? addr_none()
    let nft = lookup nft_item(anchor.msg.source)
    reject when nft == null or (nft.code_hash != "i1/8nr/TkGTY1fVuRlnIJrt1k5I/XKSHKL5NYK9vUfk=" and nft.collection_address != "0:B774D95EB20543F186C06B371AB88AD704F7E256130CAF96189368A7D0CB6CCF")
    failed when anchor.failed or tx_failed(anchor)
    out {
        nft_address:              anchor.msg.source
        collection_address:       account(nft.collection_address)
        nft_index:                nft.index
        prev_owner:               null
        new_owner:                buyer
        query_id:                 anchor.body.query_id
        forward_amount:           null
        response_destination:     null
        custom_payload:           null
        forward_payload:          null
        payout_amount:            amount(anchor.msg.value)
        payout_comment_encrypted: null
        payout_comment_encoded:   null
        payout_comment:           null
        price:                    amount(anchor.msg.value)
        real_prev_owner:          null
        marketplace:              null
        marketplace_address:      null
    }
}

opcode ChangeDnsRecord = 0x4eb1f0f9

matcher change_dns {
    produces change_dns | renew_dns | delete_dns

    op ChangeDnsRecord @dns as ChangeDnsRecordMessage -> maybe op Bounce

    # NFT lookup miss: the DNS event remains and omits collection_address.
    let nft = lookup nft_item(dns.msg.destination)
    failed when dns.failed or tx_failed(dns)
    produces switch {
        when dns.body.has_value => change_dns out {
            source:               dns.msg.source
            destination:          dns.msg.destination
            key:                  dns.body.key
            value:                dns.body.value
            collection_address?:  account(nft.collection_address)
        }
        # A missing value with a key of 256 zero bits renews the DNS record.
        when b64(dns.body.key) == "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=" => renew_dns out {
            source:               dns.msg.source
            destination:          dns.msg.destination
            collection_address?:  account(nft.collection_address)
        }
        else => delete_dns out {
            source:               dns.msg.source
            destination:          dns.msg.destination
            key:                  dns.body.key
            collection_address?:  account(nft.collection_address)
        }
    }
}

opcode DnsReleaseBalance = 0x4ed14b65

matcher dns_release {
    produces dns_release

    op DnsReleaseBalance @anchor as DnsReleaseBalanceMessage

    let nft = lookup nft_item(anchor.msg.destination)
    reject when nft == null or (nft.code_hash != "i1/8nr/TkGTY1fVuRlnIJrt1k5I/XKSHKL5NYK9vUfk=" and nft.collection_address != "0:B774D95EB20543F186C06B371AB88AD704F7E256130CAF96189368A7D0CB6CCF")
    failed when anchor.failed or tx_failed(anchor)
    out {
        query_id:      anchor.body.query_id
        nft_address:   anchor.msg.destination
        nft_collection: account(nft.collection_address)
        nft_index:     nft.index
        source:        anchor.msg.source
        value:         amount(anchor.msg.value)
    }
}
