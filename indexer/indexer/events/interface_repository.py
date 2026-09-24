from __future__ import annotations

import abc
from collections import defaultdict, deque
from contextvars import ContextVar
from dataclasses import dataclass
from typing import Optional, Callable

from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from indexer.core.database import JettonWallet, NFTItem, NftSale, NftAuction, LatestAccountState, MultisigOrder

NOMINATOR_POOL_CODE_HASH = "mj7BS8CY9rRAZMMFIiyuooAPF92oXuaoGYpwle3hDc8="

@dataclass
class DedustPool:
    address: str
    assets: dict


def _context():
    from indexer.events import context
    return context


def _to_float(value):
    if value is None:
        return None
    return float(value)


def _to_int(value):
    if value is None:
        return None
    return int(value)


def _jetton_wallet_interface(payload: dict) -> dict:
    return {
        "balance": _to_float(payload.get("balance")),
        "address": payload.get("address"),
        "owner": payload.get("owner"),
        "jetton": payload.get("jetton"),
    }


def _nft_item_interface(payload: dict) -> dict:
    return {
        "address": payload.get("address"),
        "init": payload.get("init"),
        "index": _to_float(payload.get("index")),
        "collection_address": payload.get("collection_address"),
        "owner_address": payload.get("owner_address"),
        "content": payload.get("content"),
        "code_hash": payload.get("code_hash"),
    }


def _nft_sale_interface(payload: dict) -> dict:
    return {
        "address": payload.get("address"),
        "is_complete": payload.get("is_complete"),
        "marketplace_address": payload.get("marketplace_address"),
        "nft_address": payload.get("nft_address"),
        "nft_owner_address": payload.get("nft_owner_address"),
        "full_price": _to_float(payload.get("full_price")),
        "marketplace_fee_address": payload.get("marketplace_fee_address"),
        "marketplace_fee": _to_float(payload.get("marketplace_fee")),
        "royalty_address": payload.get("royalty_address"),
        "royalty_amount": _to_float(payload.get("royalty_amount")),
        "code_hash": payload.get("code_hash"),
    }


def _nft_auction_interface(payload: dict) -> dict:
    return {
        "address": payload.get("address"),
        "mp_addr": payload.get("mp_addr"),
        "nft_addr": payload.get("nft_addr"),
        "nft_owner": payload.get("nft_owner"),
        "last_bid": _to_float(payload.get("last_bid")),
        "mp_fee_addr": payload.get("mp_fee_addr"),
        "mp_fee_factor": _to_float(payload.get("mp_fee_factor")),
        "mp_fee_base": _to_float(payload.get("mp_fee_base")),
        "royalty_fee_addr": payload.get("royalty_fee_addr"),
        "royalty_fee_base": _to_float(payload.get("royalty_fee_base")),
        "max_bid": _to_float(payload.get("max_bid")),
        "min_bid": _to_float(payload.get("min_bid")),
        "code_hash": payload.get("code_hash"),
    }


def _multisig_order_interface(payload: dict) -> dict:
    return {
        "address": payload.get("address"),
        "multisig_address": payload.get("multisig_address"),
        "order_seqno": _to_float(payload.get("order_seqno")),
        "threshold": _to_float(payload.get("threshold")),
        "sent_for_execution": payload.get("sent_for_execution"),
        "approvals_mask": _to_float(payload.get("approvals_mask")),
        "approvals_num": _to_float(payload.get("approvals_num")),
        "expiration_date": _to_float(payload.get("expiration_date")),
        "order_boc": payload.get("order_boc"),
        "signers": payload.get("signers"),
        "last_transaction_lt": _to_float(payload.get("last_transaction_lt")),
        "code_hash": payload.get("code_hash"),
        "data_hash": payload.get("data_hash"),
    }


def _dedust_pool_from_payload(payload: dict) -> DedustPool:
    assets = []
    for key in ("asset_1", "asset_2"):
        address = payload.get(key)
        assets.append({
            "is_ton": address is None,
            "address": address,
        })
    return DedustPool(address=payload.get("address"), assets=assets)


def _is_destroyed(payload: dict | None) -> bool:
    return bool(payload and payload.get("destroyed"))


def _jetton_wallet_from_interface(data: dict) -> JettonWallet:
    return JettonWallet(
        balance=data["balance"],
        address=data["address"],
        owner=data["owner"],
        jetton=data["jetton"],
    )


def _nft_item_from_interface(data: dict) -> NFTItem:
    return NFTItem(
        address=data["address"],
        init=data["init"],
        index=data["index"],
        collection_address=data["collection_address"],
        owner_address=data["owner_address"],
        content=data["content"],
        code_hash=data.get("code_hash"),
    )


def _nft_sale_from_interface(data: dict) -> NftSale:
    return NftSale(
        address=data["address"],
        is_complete=data["is_complete"],
        marketplace_address=data["marketplace_address"],
        nft_address=data["nft_address"],
        nft_owner_address=data["nft_owner_address"],
        full_price=data["full_price"],
        marketplace_fee_address=data.get("marketplace_fee_address"),
        marketplace_fee=data.get("marketplace_fee"),
        royalty_address=data.get("royalty_address"),
        royalty_amount=data.get("royalty_amount"),
        code_hash=data.get("code_hash"),
    )


def _nft_auction_from_interface(data: dict) -> NftAuction:
    return NftAuction(
        address=data["address"],
        nft_addr=data["nft_addr"],
        nft_owner=data["nft_owner"],
        last_bid=data["last_bid"],
        mp_addr=data["mp_addr"],
        mp_fee_addr=data.get("mp_fee_addr"),
        mp_fee_factor=data.get("mp_fee_factor"),
        mp_fee_base=data.get("mp_fee_base"),
        royalty_fee_addr=data.get("royalty_fee_addr"),
        royalty_fee_base=data.get("royalty_fee_base"),
        max_bid=data.get("max_bid"),
        min_bid=data.get("min_bid"),
        code_hash=data.get("code_hash"),
    )


def _multisig_order_from_interface(data: dict) -> MultisigOrder:
    return MultisigOrder(
        address=data["address"],
        multisig_address=data["multisig_address"],
        order_seqno=data["order_seqno"],
        threshold=data["threshold"],
        sent_for_execution=data["sent_for_execution"],
        approvals_mask=data["approvals_mask"],
        approvals_num=data["approvals_num"],
        expiration_date=data["expiration_date"],
        order_boc=data["order_boc"],
        signers=data["signers"],
        last_transaction_lt=data["last_transaction_lt"],
        code_hash=data["code_hash"],
        data_hash=data["data_hash"],
    )

class InterfaceRepository(abc.ABC):
    @abc.abstractmethod
    async def get_jetton_wallet(self, address: str) -> JettonWallet | None:
        pass

    @abc.abstractmethod
    async def get_nft_item(self, address: str) -> NFTItem | None:
        pass

    @abc.abstractmethod
    async def get_nft_sale(self, address: str) -> NftSale | None:
        pass

    @abc.abstractmethod
    async def get_nft_auction(self, address: str) -> NftAuction | None:
        pass

    @abc.abstractmethod
    async def get_multisig_order(self, address: str) -> MultisigOrder | None:
        pass

    @abc.abstractmethod
    async def get_interfaces(self, address: str) -> dict[str, dict]:
        pass


async def _gather_data_from_db(
        accounts: set[str],
        session: AsyncSession,
        extra_requests: list[ExtraAccountRequest] = None,
) -> tuple[list[JettonWallet], list[NFTItem], list[NftSale], list[NftAuction], list[LatestAccountState], list[MultisigOrder], list[dict]]:
    jetton_wallets = []
    nft_items = []
    nft_sales = []
    getgems_auctions = []
    nominator_pools = []
    multisig_orders = []
    extra = []
    queue = deque(extra_requests or [])
    processed = set()

    queue_iterations = 0
    while queue:
        queue_iterations += 1
        if queue_iterations > 10:
            break
        batch = defaultdict(list)
        batch_processors = {}

        while queue:
            extra_request = queue.popleft()

            if (extra_request.account, extra_request.request_type) not in processed:
                batch[extra_request.request_type].append(extra_request.account)
                batch_processors[(extra_request.account, extra_request.request_type)] = extra_request.callback
                processed.add((extra_request.account, extra_request.request_type))

        # Execute batch
        for req_type, accounts_batch in batch.items():
            if req_type == 'data_boc':
                results = await session.execute(
                    select(LatestAccountState.account, LatestAccountState.data_boc)
                    .filter(LatestAccountState.account.in_(accounts_batch))
                )

                for account, data_boc in results:
                    processor = batch_processors.get((account, req_type))
                    n_extra = {
                        'account': account,
                        'request': req_type,
                        'data_boc': data_boc
                    }
                    if processor:
                        new_requests, new_accounts = processor(n_extra)
                        queue.extend(new_requests)
                        accounts.update(new_accounts)
                    extra.append(n_extra)


    account_list = list(accounts)
    for i in range(0, len(account_list), 5000):
        batch = account_list[i:i + 5000]
        auctions = await session.execute(
            select(NftAuction)
            .filter(NftAuction.address.in_(batch))
            .filter(NftAuction.destroyed.is_(False))
        )
        auctions = list(auctions.scalars().all())
        nft_batch = batch.copy()
        for auction in auctions:
            nft_batch.append(auction.nft_addr)
        wallets = await session.execute(
            select(JettonWallet)
            .filter(JettonWallet.address.in_(batch))
            .filter(JettonWallet.destroyed.is_(False))
        )
        nft = await session.execute(
            select(NFTItem)
            .filter(NFTItem.address.in_(nft_batch))
            .filter(NFTItem.destroyed.is_(False))
        )
        sales = await session.execute(
            select(NftSale)
            .filter(NftSale.address.in_(batch))
            .filter(NftSale.destroyed.is_(False))
        )
        orders = await session.execute(
            select(MultisigOrder)
            .filter(MultisigOrder.address.in_(batch))
            .filter(MultisigOrder.destroyed.is_(False))
        )
        pools = await session.execute(select(LatestAccountState)
                                                .filter(LatestAccountState.account.in_(batch))
                                                .filter(LatestAccountState.code_hash == NOMINATOR_POOL_CODE_HASH))
        jetton_wallets += list(wallets.scalars().all())
        nft_items += list(nft.scalars().all())
        nft_sales += list(sales.scalars().all())
        getgems_auctions += auctions
        multisig_orders += list(orders.scalars().all())
        nominator_pools += list(pools.scalars().all())


    return jetton_wallets, nft_items, nft_sales, getgems_auctions, nominator_pools, multisig_orders, extra

@dataclass(frozen=True, eq=True)
class ExtraAccountRequest:
    account: str
    request_type: str = 'data_boc'
    # Callback returns (new_requests, accounts_for_interfaces)
    callback: Optional[Callable[[dict], tuple[list['ExtraAccountRequest'], set[str]]]] = None


async def gather_interfaces(accounts: set[str], session: AsyncSession, extra_requests: set[ExtraAccountRequest] = None)\
        -> dict[str, dict[str, dict]]:
    result = defaultdict(dict)
    (jetton_wallets, nft_items, nft_sales, nft_auctions, nominator_pools, multisig_orders, extra) = await _gather_data_from_db(
        accounts, session, extra_requests=extra_requests)
    for wallet in accounts:
        result[wallet] = {}
    for wallet in jetton_wallets:
        result[wallet.address]["JettonWallet"] = {
            "balance": float(wallet.balance),
            "address": wallet.address,
            "owner": wallet.owner,
            "jetton": wallet.jetton,
        }
    for item in nft_items:
        result[item.address]["NftItem"] = {
            "address": item.address,
            "init": item.init,
            "index": float(item.index),
            "collection_address": item.collection_address,
            "owner_address": item.owner_address,
            "content": item.content,
            "code_hash": item.code_hash,
        }
    for sale in nft_sales:
        result[sale.address]["NftSale"] = {
            "address": sale.address,
            "is_complete": sale.is_complete,
            "marketplace_address": sale.marketplace_address,
            "nft_address": sale.nft_address,
            "nft_owner_address": sale.nft_owner_address,
            "full_price": float(sale.full_price),
            "marketplace_fee_address": sale.marketplace_fee_address,
            "marketplace_fee": float(sale.marketplace_fee) if sale.marketplace_fee is not None else None,
            "royalty_address": sale.royalty_address,
            "royalty_amount": float(sale.royalty_amount) if sale.royalty_amount is not None else None,
            "code_hash": sale.code_hash,
        }
    for auction in nft_auctions:
        result[auction.address]["NftAuction"] = {
            "address": auction.address,
            "mp_addr": auction.mp_addr,
            "nft_addr": auction.nft_addr,
            "nft_owner": auction.nft_owner,
            "last_bid": float(auction.last_bid),
            "mp_fee_addr": auction.mp_fee_addr,
            "mp_fee_factor": float(auction.mp_fee_factor) if auction.mp_fee_factor is not None else None,
            "mp_fee_base": float(auction.mp_fee_base) if auction.mp_fee_base is not None else None,
            "royalty_fee_addr": auction.royalty_fee_addr,
            "royalty_fee_base": float(auction.royalty_fee_base) if auction.royalty_fee_base is not None else None,
            "max_bid": float(auction.max_bid) if auction.max_bid is not None else None,
            "min_bid": float(auction.min_bid) if auction.min_bid is not None else None,
            "code_hash": auction.code_hash,
        }
    for account_state in nominator_pools:
        result[account_state.account]["NominatorPool"] = {
            "address": account_state.account,
        }
    for order in multisig_orders:
        result[order.address]["MultisigOrder"] = {
            "address": order.address,
            "multisig_address": order.multisig_address,
            "order_seqno": float(order.order_seqno),
            "threshold": float(order.threshold),
            "sent_for_execution": order.sent_for_execution,
            "approvals_mask": float(order.approvals_mask),
            "approvals_num": float(order.approvals_num),
            "expiration_date": float(order.expiration_date),
            "order_boc": order.order_boc,
            "signers": order.signers,
            "last_transaction_lt": float(order.last_transaction_lt),
            "code_hash": order.code_hash,
            "data_hash": order.data_hash,
        }
    for wallet in extra:
        result[wallet['account']][wallet['request']] = wallet
    return result
