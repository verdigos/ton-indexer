"""mch registration for the GetGems/Fragment sale + auction family
(specs/nft_sale.mch).

The eight matchers in that spec are declarative; this module registers the names
they reference — opcodes, produced btypes, the two message parsers, the
nft_sale / nft_auction / nft_item lookups, six predicates and four host fns.

Why each host binding exists (the spec header lists the same rationale from the
DSL side):

* ``getgems_sale_init`` / ``getgems_auction_init`` read the sale parameters out
  of the DEPLOY MESSAGE's StateInit, selecting a parser by the contract's
  ``code_hash``. Both delegate to the legacy ``get_sale_data`` /
  ``get_auction_data`` (blocks/messages/getgems.py) verbatim, which makes this
  leg parity-by-construction; the C++ leg (host/HostNftSale.cpp) repeats the same
  version tables and field walks.
* ``auction_bid_data`` is the whole of ``AuctionBidMatcher.build_block``. It has
  to be a host fn because the getgems branch looks an NFT ITEM up by an address
  that came out of the AUCTION lookup — a lookup keyed on a lookup, which the
  build language forbids (mch-spec §9.7, ``B004_LOOKUP_DEPENDS_ON_LOOKUP``).
* ``nft_trade_is_finish`` is the ``produces switch`` discriminator for
  NftCancelAuctionMatcher's finish-vs-cancel outcome, which tests an opcode SET
  on one anchor branch and a comment SET on another.
* ``auction_outbid_unique`` is the "exactly one, reject on duplicate" quantifier
  ``AuctionOutbidMatcher`` applies to its outbid sibling. The language has no
  arity form, so the count lives on the anchor predicate and the pattern then
  binds whichever leg ``auction_outbid_leg`` accepts.

Predicates read ``block.data`` rather than ``isinstance``: the IR engine's
declarative products are plain btype+dict blocks, so an isinstance test against a
legacy Block subclass is dead code there.

The legacy matchers in blocks/auction.py (UpdateSaleMatcher,
NftPutOnSaleBlockMatcher, NftCancelSaleMatcher, NftFinishAuctionMatcher,
NftCancelAuctionMatcher, AuctionBidMatcher, AuctionOutbidMatcher) and
blocks/nft.py (NftDiscoveryBlockMatcher) stay the parity reference.
"""
from __future__ import annotations

from types import SimpleNamespace
from typing import Any

from indexer.events import context
from indexer.events.blocks.auction import (
    AuctionBid,
    AuctionOutbidBlock,
    NftCancelAuctionBlock,
    NftCancelSaleBlock,
    NftFinishAuctionBlock,
    NftPutOnAuctionBlock,
    NftPutOnSaleBlock,
    UpdateSaleBlock,
    _is_dns_item,
    _is_teleitem,
)
from indexer.events.blocks.messages.getgems import (
    SaleUpdateMessage,
    get_auction_data,
    get_sale_data,
)
from indexer.events.blocks.messages.nft import NftReportStaticData
from indexer.events.blocks.nft import NftDiscoveryBlock
from indexer.events.blocks.utils import AccountId, Amount
from indexer.events.mch.registry import Registries

# 0x3 is GetGems' bare "cancel sale" op; 0x557cea20 is the teleitem outbid
# notification the fragment branch of AuctionOutbid consumes.
OPCODES = {
    "SaleUpdate":          SaleUpdateMessage.opcode,   # 0x6c6c2080
    "NftDiscovery":        0x2FCB26A2,
    "NftReportStaticData": NftReportStaticData.opcode,  # 0x8b771735
    "NftSaleCancel":       0x3,
    "NftAuctionCancel":    0x5616C572,
    "NftAuctionFinish":    0x20C9EB18,
    "NftAuctionStop":      0xB95616B6,
    "TeleitemOutbid":      0x557CEA20,
}

# Registry keys are the `produces` btypes in specs/nft_sale.mch.
BLOCK_TYPES = {
    "nft_update_sale":     UpdateSaleBlock,
    "nft_put_on_sale":     NftPutOnSaleBlock,
    "nft_put_on_auction":  NftPutOnAuctionBlock,
    "nft_discovery":       NftDiscoveryBlock,
    "auction_bid":         AuctionBid,
    "auction_outbid":      AuctionOutbidBlock,
    "nft_cancel_sale":     NftCancelSaleBlock,
    "nft_cancel_auction":  NftCancelAuctionBlock,
    "nft_finish_auction":  NftFinishAuctionBlock,
}

MESSAGE_TYPES = {
    "SaleUpdateMessage":   SaleUpdateMessage,
    "NftReportStaticData": NftReportStaticData,
}

_CANCEL_COMMENTS = ("cancel", "finish", "stop")
_FINISH_COMMENTS = ("finish", "stop")
_FINISH_OPCODES = (0xB95616B6, 0x20C9EB18)

# The fixed key sets the two StateInit readers hand back. out{} decides which
# ones a branch emits; a key the parser could not fill is None.
_SALE_KEYS = ("full_price", "marketplace_address", "marketplace_fee_address",
              "marketplace_fee", "royalty_address", "royalty_amount")
_AUCTION_KEYS = ("mp_fee_addr", "mp_fee_factor", "mp_fee_base",
                 "royalty_fee_addr", "royalty_fee_base", "max_bid", "min_bid")


def _dfield(block, key):
    data = getattr(block, "data", None)
    if isinstance(data, dict):
        return data.get(key)
    return getattr(data, key, None)


def _comment(block) -> str | None:
    c = _dfield(block, "comment")
    return c if isinstance(c, str) else None


def _has_deployment(block) -> bool:
    return bool(getattr(block, "contract_deployments", None))


def _init_state_boc(block) -> str | None:
    msg = block.get_message() if block is not None else None
    init = getattr(msg, "init_state", None) if msg is not None else None
    return getattr(init, "body", None) if init is not None else None


def _opt_int(v):
    return int(v) if v is not None else None


def register_nft_sale(reg: Registries) -> Registries:
    """Register everything specs/nft_sale.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)
    reg.message_types.update(MESSAGE_TYPES)

    # Raw (un-normalized) lookup keys, like builders/dns.py and
    # builders/teleitem_auction.py: every legacy caller here passes
    # `block.get_message().destination` with no `.upper()`.
    @reg.register_lookup("nft_sale")
    async def nft_sale(address: str) -> Any:
        repo = context.interface_repository.get()
        return await repo.get_nft_sale(address) if repo is not None else None

    @reg.register_lookup("nft_auction")
    async def nft_auction(address: str) -> Any:
        repo = context.interface_repository.get()
        return await repo.get_nft_auction(address) if repo is not None else None

    @reg.register_lookup("nft_item")
    async def nft_item(address: str) -> Any:
        repo = context.interface_repository.get()
        return await repo.get_nft_item(address) if repo is not None else None

    # --- predicates ---------------------------------------------------------

    @reg.register_predicate("sale_contract_deploy")
    def sale_contract_deploy(block) -> bool:
        # blocks/auction.py:237 — note `> 0`, deliberately weaker than
        # nft_mint's `single_contract_deploy` (`== 1`).
        return block.btype in ("ton_transfer", "call_contract") and _has_deployment(block)

    @reg.register_predicate("nft_trade_cancel_comment")
    def nft_trade_cancel_comment(block) -> bool:
        return block.btype == "ton_transfer" and _comment(block) in _CANCEL_COMMENTS

    @reg.register_predicate("nft_trade_finish_comment")
    def nft_trade_finish_comment(block) -> bool:
        return block.btype == "ton_transfer" and _comment(block) in _FINISH_COMMENTS

    @reg.register_predicate("auction_bid_candidate")
    def auction_bid_candidate(block) -> bool:
        # The two cheap guards AuctionBidMatcher runs before it touches the
        # interface repository (blocks/auction.py:65-69).
        return (block.btype == "ton_transfer" and not _has_deployment(block)
                and _comment(block) not in _CANCEL_COMMENTS)

    def _is_outbid_leg(block) -> bool:
        bid = getattr(block, "previous_block", None)
        if bid is None or bid.btype != "auction_bid":
            return False
        auction = _dfield(bid, "auction")
        kind = _dfield(bid, "auction_type")
        if auction is None or auction != _dfield(block, "source"):
            return False
        if block.btype == "ton_transfer":
            comment = _comment(block) or ""
            return (kind == "getgems"
                    and "Your bid has been outbid by another user" in comment)
        return kind == "fragment" and getattr(block, "opcode", None) == 0x557CEA20

    reg.register_predicate("auction_outbid_leg")(_is_outbid_leg)

    # --- host fns -----------------------------------------------------------

    @reg.register_fn("getgems_sale_init")
    async def getgems_sale_init(sale_init, code_hash):
        boc = _init_state_boc(sale_init)
        if boc is None:
            return None
        data = get_sale_data(boc, code_hash)
        if data is None:
            return None
        return SimpleNamespace(
            full_price=data.full_price,
            marketplace_address=data.marketplace_address,
            marketplace_fee_address=data.marketplace_fee_address,
            marketplace_fee=_opt_int(data.marketplace_fee),
            royalty_address=data.royalty_address,
            royalty_amount=_opt_int(data.royalty_amount),
        )

    @reg.register_fn("getgems_auction_init")
    async def getgems_auction_init(sale_init, code_hash):
        boc = _init_state_boc(sale_init)
        if boc is None:
            return None
        data = get_auction_data(boc, code_hash)
        if data is None:
            return None
        return SimpleNamespace(
            mp_fee_addr=data.mp_fee_addr,
            mp_fee_factor=_opt_int(data.mp_fee_factor),
            mp_fee_base=_opt_int(data.mp_fee_base),
            royalty_fee_addr=data.royalty_fee_addr,
            royalty_fee_base=_opt_int(data.royalty_fee_base),
            max_bid=_opt_int(data.max_bid),
            min_bid=_opt_int(data.min_bid),
        )

    @reg.register_fn("auction_bid_data")
    async def auction_bid_data(bid):
        msg = bid.get_message()
        destination = msg.destination
        if destination is None:
            return None
        repo = context.interface_repository.get()
        if repo is None:
            return None
        common = dict(amount=Amount(msg.value), bidder=AccountId(msg.source),
                      auction=AccountId(destination))

        auction = await repo.get_nft_auction(destination)
        if auction is not None:
            item = await repo.get_nft_item(auction.nft_addr)
            return SimpleNamespace(
                **common,
                nft_address=AccountId(auction.nft_addr),
                nft_item_index=item.index if item is not None else None,
                nft_collection=(AccountId(item.collection_address)
                                if item is not None else None),
                auction_type="getgems",
            )

        item = await repo.get_nft_item(destination)
        if item is None:
            return None
        dns = _is_dns_item(item)
        if not dns and not _is_teleitem(item):
            return None
        if dns:
            # A DNS bid counts only when it actually outbid someone; legacy
            # requires the teleitem outbid notification among the children.
            if not any(getattr(b, "opcode", None) == 0x557CEA20 for b in bid.next_blocks):
                return None
        return SimpleNamespace(
            **common,
            nft_address=AccountId(destination),
            nft_item_index=item.index,
            nft_collection=(AccountId(item.collection_address)
                            if item.collection_address is not None else None),
            auction_type="fragment",
        )

    @reg.register_fn("nft_trade_returned")
    async def nft_trade_returned(anchor, btype):
        # The child the legacy matcher GATES on but does NOT merge: its
        # build_block merges only `[block]`, leaving the child a top-level spine
        # row. mch consumption IS the pattern match, so a pattern edge would
        # swallow it and demote that row to a child row — hence this fn instead
        # of an edge. First match in next_blocks order, like BlockTypeMatcher.
        if anchor is None:
            return None
        for nxt in anchor.next_blocks:
            if nxt.btype != btype:
                return_nft = None
            else:
                return_nft = nxt
            if return_nft is None:
                continue
            if btype == "nft_transfer":
                nft = _dfield(return_nft, "nft") or {}
                collection = nft.get("collection") if isinstance(nft, dict) else None
                return SimpleNamespace(
                    nft_address=nft.get("address") if isinstance(nft, dict) else None,
                    # Legacy get_nft_collection() indexes into a None collection
                    # and raises; null-propagating here is the documented
                    # deviation (a row with a null collection vs no row).
                    nft_collection=(collection or {}).get("address"),
                    owner=_dfield(return_nft, "new_owner"),
                )
            return SimpleNamespace(
                nft_address=_dfield(return_nft, "nft_address"),
                nft_collection=_dfield(return_nft, "collection_address"),
                owner=_dfield(return_nft, "new_owner"),
            )
        return None

    @reg.register_fn("auction_outbid_data")
    async def auction_outbid_data(leg):
        # AuctionOutbidMatcher's whole build. The matcher anchors on the LEG (see
        # the .mch header): legacy merges only the leg and leaves the auction_bid
        # a spine block, which an anchor edge would consume. The bid is reached
        # through .previous_block, and the "exactly one, reject on duplicate"
        # count lives here too.
        bid = getattr(leg, "previous_block", None) if leg is not None else None
        if bid is None:
            return None
        n = 0
        for sib in bid.next_blocks:
            if _is_outbid_leg(sib):
                n += 1
                if n > 1:
                    return None
        if n != 1:
            return None
        msg = leg.get_message()
        getgems = leg.btype == "ton_transfer"
        return SimpleNamespace(
            auction_address=_dfield(bid, "auction"),
            nft=_dfield(bid, "nft_address"),
            nft_collection=_dfield(bid, "nft_collection"),
            bidder=AccountId(msg.destination),
            new_bidder=_dfield(bid, "bidder"),
            amount=Amount(msg.value),
            # The fragment leg carries no comment (legacy hardcodes None).
            comment=_comment(leg) if getgems else None,
            auction_type=_dfield(bid, "auction_type"),
        )

    @reg.register_fn("nft_trade_is_finish")
    async def nft_trade_is_finish(block) -> bool:
        if block is None:
            return False
        if block.btype == "ton_transfer":
            return _comment(block) in _FINISH_COMMENTS
        return getattr(block, "opcode", None) in _FINISH_OPCODES

    return reg


def nft_sale_registries() -> Registries:
    """Fresh Registries ready to compile specs/nft_sale.mch."""
    return register_nft_sale(Registries())
