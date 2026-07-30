"""mch registration for the teleitem auction matchers (specs/teleitem_auction.mch).

specs/teleitem_auction.mch is FULLY DECLARATIVE: the runtime evaluates it, so
this module only registers the names it references — the opcodes, the produced
btypes, the `TeleitemStartAuction` body parser, an `nft_item` lookup and the one
host fn the expression language cannot express.

`is_teleitem` is that fn. The legacy gate (`blocks/auction.py::_is_teleitem`) is a
SUBSTRING test on the item's metadata `uri`, and the spec forbids string
manipulation in the expression language (mch-spec §9.6: "no string manipulation.
If a computation does not fit, it belongs in a host `fn`"), so the predicate
stays a one-line host fn on both sides. It delegates to the legacy `_is_teleitem`
verbatim, which makes the Python leg parity-by-construction; the C++ leg
(host/HostNft.cpp) repeats the same `content["uri"] contains
"https://nft.fragment.com"` test the nft-transfer core already runs there.

The `nft_item` lookup is the RAW (un-normalized) variant, exactly like
builders/dns.py: the legacy matchers call
`get_nft_item(block.get_message().destination)` with no `.upper()`.

The legacy TeleitemStartAuctionMatcher / TeleitemCancelAuctionMatcher
(blocks/auction.py) stay the parity reference.
"""
from __future__ import annotations

from typing import Any

from indexer.events import context
from indexer.events.blocks.auction import (
    TeleitemCancelAuctionBlock,
    TeleitemStartAuctionBlock,
    _is_teleitem,
)
from indexer.events.blocks.messages.nft import TeleitemStartAuction
from indexer.events.mch.registry import Registries

OPCODES = {
    "TeleitemStartAuction":  TeleitemStartAuction.opcode,  # 0x487a8e81
    "TeleitemCancelAuction": 0x371638AE,
    # The auction contract's "ok" reply, consumed as the anchor's optional child
    # (the legacy `labeled('teleitem_ok', ContractMatcher(0xa37a0983, optional=True))`).
    "TeleitemOk":            0xA37A0983,
}

# Registry keys are the `produces` btypes in specs/teleitem_auction.mch.
BLOCK_TYPES = {
    "teleitem_start_auction":  TeleitemStartAuctionBlock,
    "teleitem_cancel_auction": TeleitemCancelAuctionBlock,
}

MESSAGE_TYPES = {
    "TeleitemStartAuction": TeleitemStartAuction,
}


def register_teleitem_auction(reg: Registries) -> Registries:
    """Register everything specs/teleitem_auction.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)
    reg.message_types.update(MESSAGE_TYPES)

    @reg.register_lookup("nft_item")
    async def nft_item(address: str) -> Any:
        # Raw (un-normalized) lookup — matches the legacy matchers'
        # `get_nft_item(destination)`. Do NOT `.upper()` here.
        repo = context.interface_repository.get()
        if repo is None:
            return None
        return await repo.get_nft_item(address)

    @reg.register_fn("is_teleitem")
    async def is_teleitem(nft: Any) -> bool:
        # Nulls pass through to host fns (spec §9.10), and _is_teleitem answers
        # False for a missing content — same rejection the legacy build takes.
        return bool(_is_teleitem(nft)) if nft is not None else False

    return reg


def teleitem_auction_registries() -> Registries:
    """Fresh Registries ready to compile specs/teleitem_auction.mch."""
    return register_teleitem_auction(Registries())
