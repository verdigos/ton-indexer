"""mch builder for the ethena deposit matcher (specs/ethena.mch).

Thin adapter: extracts the captured jetton_transfer + jetton_mint blocks from
the runtime Match and delegates to the shared build core in
blocks/ethena_staking.py — the same function the legacy EthenaDepositBlockMatcher
calls. The core applies the hardcoded-asset gate and returns None to reject the
match (spec §10.7); the synthesized wrapper merges the consumed blocks.
"""
from __future__ import annotations

from indexer.events.blocks.ethena_staking import EthenaDepositBlock
from indexer.events.mch.registry import Registries

BLOCK_TYPES = {
    "EthenaDepositBlock": EthenaDepositBlock,
}


def register_ethena(reg: Registries) -> Registries:
    """Register everything specs/ethena.mch needs into `reg`.

    specs/ethena.mch is FULLY DECLARATIVE: the hardcoded USDe/tsUSDe asset gate
    is `reject when` clauses and every field is read off the composed
    jetton_transfer / jetton_mint blocks' `data`. No builder to register.
    """
    reg.block_types.update(BLOCK_TYPES)
    return reg


def ethena_registries() -> Registries:
    """Fresh Registries ready to compile specs/ethena.mch."""
    return register_ethena(Registries())
