"""Host-side builder registrations for `.mch` matchers.

Each protocol module exposes `register_*` (populate an existing Registries)
and a `*_registries` factory (fresh Registries ready to compile the matching
spec file in `indexer/events/mch/specs/`).
"""
from __future__ import annotations

from indexer.events.mch.builders.coffee import coffee_registries, register_coffee
from indexer.events.mch.builders.dedust import (
    dedust_hybrid_registries,
    dedust_registries,
    dedust_withdraw_registries,
    register_dedust,
    register_dedust_hybrid,
    register_dedust_withdraw,
)
from indexer.events.mch.builders.dedust_v2 import (
    dedust_v2_claim_reward_registries,
    register_dedust_v2_claim_reward,
)
from indexer.events.mch.builders.dns import dns_registries, register_dns
from indexer.events.mch.builders.jettons import (
    jetton_decl_registries,
    jetton_registries,
    register_jettons,
)
from indexer.events.mch.builders.nft_mint import (
    nft_mint_registries,
    register_nft_mint,
)
from indexer.events.mch.builders.stonfi import (
    register_stonfi_v2,
    stonfi_v2_registries,
)
from indexer.events.mch.builders.stonfi_liquidity import (
    register_stonfi_liquidity,
    stonfi_liquidity_registries,
)
from indexer.events.mch.builders.stonfi_provide import (
    register_stonfi_provide,
    stonfi_provide_registries,
)
from indexer.events.mch.builders.stonfi_v1 import (
    register_stonfi_v1,
    stonfi_v1_registries,
)
from indexer.events.mch.builders.teleitem_auction import (
    register_teleitem_auction,
    teleitem_auction_registries,
)

__all__ = [
    "coffee_registries",
    "dedust_hybrid_registries",
    "dedust_registries",
    "dedust_withdraw_registries",
    "dedust_v2_claim_reward_registries",
    "dns_registries",
    "jetton_decl_registries",
    "jetton_registries",
    "nft_mint_registries",
    "register_coffee",
    "register_dedust",
    "register_dedust_hybrid",
    "register_dedust_withdraw",
    "register_dedust_v2_claim_reward",
    "register_dns",
    "register_jettons",
    "register_nft_mint",
    "register_stonfi_liquidity",
    "register_stonfi_provide",
    "register_stonfi_v1",
    "register_stonfi_v2",
    "register_teleitem_auction",
    "stonfi_liquidity_registries",
    "stonfi_provide_registries",
    "stonfi_v1_registries",
    "stonfi_v2_registries",
    "teleitem_auction_registries",
]
