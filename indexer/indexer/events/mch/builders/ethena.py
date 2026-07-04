"""mch builder for the ethena deposit matcher (specs/ethena.mch).

Thin adapter: extracts the captured jetton_transfer + jetton_mint blocks from
the runtime Match and delegates to the shared build core in
blocks/ethena_staking.py — the same function the legacy EthenaDepositBlockMatcher
calls. The core applies the hardcoded-asset gate and returns None to reject the
match (spec §10.7); the synthesized wrapper merges the consumed blocks.
"""
from __future__ import annotations

from indexer.events.blocks.ethena_staking import (
    EthenaDepositBlock,
    build_ethena_deposit_core,
)
from indexer.events.mch.registry import Registries

BLOCK_TYPES = {
    "EthenaDepositBlock": EthenaDepositBlock,
}


def register_ethena(reg: Registries) -> Registries:
    """Register everything specs/ethena.mch needs into `reg`."""
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_builder("build_ethena_deposit")
    async def build_ethena_deposit(m):
        return build_ethena_deposit_core(m.transfer, m.mint)

    return reg


def ethena_registries() -> Registries:
    """Fresh Registries ready to compile specs/ethena.mch."""
    return register_ethena(Registries())
