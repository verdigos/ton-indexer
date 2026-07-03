"""mch builder for the DNS record-change matcher (specs/dns.mch).

Thin adapter: extracts the anchor block from the runtime Match and delegates to
the shared build core in blocks/dns.py — the same function the legacy
ChangeDnsRecordMatcher calls. The core dispatches on parsed message content and
returns one of change_dns / renew_dns / delete_dns (the produces-union of
specs/dns.mch); the synthesized wrapper merges the consumed blocks.
"""
from __future__ import annotations

from indexer.events.blocks.dns import (
    ChangeDnsRecordBlock,
    DeleteDnsRecordBlock,
    DnsRenewBlock,
    build_change_dns_core,
)
from indexer.events.blocks.messages.dns import ChangeDnsRecordMessage
from indexer.events.mch.registry import Registries

OPCODES = {
    "ChangeDnsRecord": ChangeDnsRecordMessage.opcode,
}

# Registry keys are the produce-union names used in specs/dns.mch
# (`produces change_dns | renew_dns | delete_dns`) — the block btypes, matching
# the declarative `produces switch` names in spec §14.5.
BLOCK_TYPES = {
    "change_dns": ChangeDnsRecordBlock,
    "renew_dns":  DnsRenewBlock,
    "delete_dns": DeleteDnsRecordBlock,
}


def register_dns(reg: Registries) -> Registries:
    """Register everything specs/dns.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_builder("build_change_dns")
    async def build_change_dns(m):
        return await build_change_dns_core(m.msg)

    return reg


def dns_registries() -> Registries:
    """Fresh Registries ready to compile specs/dns.mch."""
    return register_dns(Registries())
