# DNS record-change matcher (T14) — ported from blocks/dns.py.
# One anchor opcode (ChangeDnsRecord), three semantic outcomes dispatched on
# parsed message content by the host build core: change_dns / renew_dns /
# delete_dns (spec §8.1 produces-union). The optional trailing bounce
# (op 0xffffffff) is consumed as an explicit child; include_excess/include_bounces
# stay at their defaults (true) to mirror the legacy matcher's consumed set.
#
# The fully declarative `produces switch { ... }` form is M4 (spec §14.5);
# under T14 the build stays host-side (mch/builders/dns.py).

opcode ChangeDnsRecord = 0x4eb1f0f9

matcher change_dns {
    produces change_dns | renew_dns | delete_dns
    build    build_change_dns
    entry    @msg

    op ChangeDnsRecord @msg -> maybe op 0xffffffff
}
