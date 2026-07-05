# DNS record-change matcher — ported from blocks/dns.py, now FULLY DECLARATIVE
# (M4 §9.5 `produces switch`). One anchor opcode (ChangeDnsRecord), three
# content-dispatched outcomes: change_dns / renew_dns / delete_dns. The optional
# trailing bounce (op 0xffffffff) is consumed as an explicit child;
# include_excess/include_bounces stay at their defaults (true) to mirror the
# legacy consumed set.
#
# The switch dispatches exactly like build_change_dns_core: a present value ->
# change_dns; else a zero key -> renew_dns; else -> delete_dns. `collection_address`
# is an OPTIONAL key (`name?: expr`) — emitted only when the destination resolves
# to an NftItem, mirroring the legacy conditional `data['collection_address'] = …`.

opcode ChangeDnsRecord = 0x4eb1f0f9

matcher change_dns {
    produces change_dns | renew_dns | delete_dns
    entry    @dns

    op ChangeDnsRecord @dns -> maybe op 0xffffffff

    parse dns as ChangeDnsRecordMessage
    let nft = lookup nft_item(dns.msg.destination)
    failed when dns.failed
    produces switch {
        when dns.body.has_value => change_dns out {
            source:               account(dns.msg.source)
            destination:          account(dns.msg.destination)
            key:                  dns.body.key
            value:                dns.body.value
            collection_address?:  account(nft.collection_address)
        }
        # zero key (b64 of 32 zero bytes) with no value -> renew.
        when b64(dns.body.key) == "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=" => renew_dns out {
            source:               account(dns.msg.source)
            destination:          account(dns.msg.destination)
            collection_address?:  account(nft.collection_address)
        }
        else => delete_dns out {
            source:               account(dns.msg.source)
            destination:          account(dns.msg.destination)
            key:                  dns.body.key
            collection_address?:  account(nft.collection_address)
        }
    }
}
