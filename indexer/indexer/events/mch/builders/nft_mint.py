"""Register the nft_mint matcher from specs/nft_mint.mch.

The runtime evaluates the spec, so this module registers only the produced btype,
`nft_item` lookup and the one host predicate the pattern language cannot
express.

`single_contract_deploy` is that predicate. `Block.contract_deployments` is a
property computed over the block's event nodes (blocks/core.py
`_find_contract_deployments`), not a field of the block's `data` map, so the
inline `where (.field == literal)` form cannot reach it. It therefore uses a
`pred` anchor implemented by the same one-line host predicate in both engines.

The `nft_item` lookup uses the raw, unnormalized address, like
builders/dns.py and builders/teleitem_auction.py. The legacy matcher looks up
`next(iter(block.contract_deployments)).as_str()`, i.e. `AccountId(tx.account)`
in raw-uppercase form. `transaction.account` is stored in that form, so the
round trip is the identity and the three builders share one lookup body. Their
registries are merged by `combined_registries`, with the last value winning.
"""
from __future__ import annotations

from typing import Any

from indexer.events import context
from indexer.events.blocks.nft import NftMintBlock
from indexer.events.mch.registry import Registries

# Registry keys are the `produces` btypes in specs/nft_mint.mch.
BLOCK_TYPES = {
    "nft_mint": NftMintBlock,
}


def register_nft_mint(reg: Registries) -> Registries:
    """Register everything specs/nft_mint.mch needs into `reg`."""
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_predicate("single_contract_deploy")
    def single_contract_deploy(block: Any) -> bool:
        # blocks/nft.py:442-443 verbatim. Sync and pure.
        return len(block.contract_deployments) == 1

    @reg.register_lookup("nft_item")
    async def nft_item(address: str) -> Any:
        # Raw (un-normalized) lookup — see the module docstring. Do NOT `.upper()`.
        repo = context.interface_repository.get()
        if repo is None:
            return None
        return await repo.get_nft_item(address)

    return reg


def nft_mint_registries() -> Registries:
    """Fresh Registries ready to compile specs/nft_mint.mch."""
    return register_nft_mint(Registries())
