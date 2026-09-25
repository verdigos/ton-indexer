from __future__ import annotations

import abc
from dataclasses import dataclass

from indexer.core.database import JettonWallet, NFTItem, NftSale, NftAuction, MultisigOrder

@dataclass
class DedustPool:
    address: str
    assets: dict


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
