"""mch registration for the DNS record-change matcher (specs/dns.mch).

specs/dns.mch is FULLY DECLARATIVE (M4 §9.5 `produces switch`): the runtime
evaluates it, so this module registers the opcodes, the produced block types,
the message parser (for `parse`), and an `nft_item` lookup. The lookup mirrors
the legacy `build_change_dns_core` exactly — it queries the interface
repository with the raw destination address (NO `.upper()` normalization,
unlike the shared default_lookups used by the jetton specs), so the declarative
collection_address resolves identically to the legacy conditional add. The
legacy ChangeDnsRecordMatcher and build_change_dns_core stay the parity
reference.
"""
from __future__ import annotations

from typing import Any

from indexer.events import context
from indexer.events.blocks.dns import (
    ChangeDnsRecordBlock,
    DeleteDnsRecordBlock,
    DnsRenewBlock,
)
from indexer.events.blocks.messages.dns import ChangeDnsRecordMessage
from indexer.events.mch.registry import Registries

OPCODES = {
    "ChangeDnsRecord": ChangeDnsRecordMessage.opcode,
}

# Registry keys are the produce-union / `produces switch` btypes in specs/dns.mch.
BLOCK_TYPES = {
    "change_dns": ChangeDnsRecordBlock,
    "renew_dns":  DnsRenewBlock,
    "delete_dns": DeleteDnsRecordBlock,
}

MESSAGE_TYPES = {
    "ChangeDnsRecordMessage": ChangeDnsRecordMessage,
}


def register_dns(reg: Registries) -> Registries:
    """Register everything specs/dns.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)
    reg.message_types.update(MESSAGE_TYPES)

    @reg.register_lookup("nft_item")
    async def nft_item(address: str) -> Any:
        # Raw (un-normalized) lookup — matches legacy build_change_dns_core's
        # `get_nft_item(destination)`. Do NOT `.upper()` here.
        repo = context.interface_repository.get()
        if repo is None:
            return None
        return await repo.get_nft_item(address)

    return reg


def dns_registries() -> Registries:
    """Fresh Registries ready to compile specs/dns.mch."""
    return register_dns(Registries())
