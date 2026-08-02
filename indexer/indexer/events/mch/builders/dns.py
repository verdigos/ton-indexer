"""Register the DNS record-change matcher from specs/dns.mch.

The spec uses `produces switch` for its build. This module registers its
opcodes, block types, message parser, and `nft_item` lookup. The lookup mirrors
`build_change_dns_core`: it queries the interface repository with the raw
destination address, without the `.upper()` normalization used by the shared
jetton lookups. This keeps collection-address resolution consistent with the
legacy builder.
"""
from __future__ import annotations

from typing import Any

from indexer.events import context
from indexer.events.blocks.dns import (
    ChangeDnsRecordBlock,
    DeleteDnsRecordBlock,
    DnsRenewBlock,
)
from indexer.events.blocks.auction import DnsPurchaseBlock, DnsReleaseBlock
from indexer.events.blocks.messages.dns import ChangeDnsRecordMessage
from indexer.events.blocks.messages.nft import AuctionFillUp, DnsReleaseBalance
from indexer.events.blocks.utils import AccountId
from indexer.events.mch.registry import Registries

OPCODES = {
    "ChangeDnsRecord": ChangeDnsRecordMessage.opcode,
    "AuctionFillUp": AuctionFillUp.opcode,
    "DnsReleaseBalance": DnsReleaseBalance.opcode,
}

# Registry keys are the produce-union / `produces switch` btypes in specs/dns.mch.
BLOCK_TYPES = {
    "change_dns": ChangeDnsRecordBlock,
    "renew_dns":  DnsRenewBlock,
    "delete_dns": DeleteDnsRecordBlock,
    "dns_purchase": DnsPurchaseBlock,
    "dns_release": DnsReleaseBlock,
}

MESSAGE_TYPES = {
    "ChangeDnsRecordMessage": ChangeDnsRecordMessage,
    "AuctionFillUpMessage": AuctionFillUp,
    "DnsReleaseBalanceMessage": DnsReleaseBalance,
}


def register_dns(reg: Registries) -> Registries:
    """Register everything specs/dns.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)
    reg.message_types.update(MESSAGE_TYPES)

    @reg.register_fn("dns_purchase_buyer")
    async def dns_purchase_buyer(anchor):
        parent = anchor.previous_block if anchor is not None else None
        if parent is None or parent.btype != "call_contract":
            return None
        return AccountId(parent.get_message().source)

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
