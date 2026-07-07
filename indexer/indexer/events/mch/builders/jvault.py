"""mch registration for the jvault family (specs/jvault.mch) — R02 declarative.

specs/jvault.mch is now DECLARATIVE (M4 §9): the runtime evaluates
parse/lookup/reject/failed/out field-for-field with the legacy cores in
blocks/jvault.py, so this module registers only the produced block types, the
message parsers (`parse`), the record-returning assets lookup, and the two
host fns the language cannot express.

Registrations and the GAP each one represents (spec §9.10 — every host hook is
a dual-implementation parity liability):

- lookup `jvault_assets` (record, spec §15b G10): wraps extract_jvault_assets
  (blocks/jvault.py) — two chained interface-repo reads plus a data-cell parse
  — and returns ONE record {staking_pool, asset, jvault_asset} or null. This is
  the sanctioned multi-value-as-record shape: the build reads `assets.asset`
  etc. and rejects on null. First real use of the pattern.
- fn `jvault_stake_period`: parses the stake period out of the jetton-transfer
  forward_payload sub-cell. Protocol-specific cell parsing — the sanctioned
  irreducible escape (§15b), not a data field an inline test could reach.
- fn `jvault_account_list`: maps the claim body's jetton-wallet address list
  through the AccountId constructor. Needed only because element-wise
  constructor-map (`map(xs as e => account(e))`) is deferred G-LC (§16.1);
  `map(xs, "field")` projects a field, it cannot apply a constructor.

The legacy JVault*BlockMatcher and the build_*_core functions stay the parity
reference (blocks/jvault.py).
"""
from __future__ import annotations

from pytoniq_core import Cell

from indexer.events.blocks.core import Block
from indexer.events.blocks.jvault import (
    JVaultClaimBlock,
    JVaultStakeBlock,
    JVaultUnstakeBlock,
    JVaultUnstakeRequestBlock,
    extract_jvault_assets,
)
from indexer.events.blocks.messages.jvault import (
    JVaultClaim,
    JVaultUnstakeJettons,
    JVaultUnstakeRequest,
)
from indexer.events.blocks.utils import AccountId
from indexer.events.mch.registry import Registries

BLOCK_TYPES = {
    "jvault_stake": JVaultStakeBlock,
    "jvault_unstake": JVaultUnstakeBlock,
    "jvault_claim": JVaultClaimBlock,
    "jvault_unstake_request": JVaultUnstakeRequestBlock,
}

MESSAGE_TYPES = {
    "JVaultUnstakeJettons": JVaultUnstakeJettons,
    "JVaultUnstakeRequest": JVaultUnstakeRequest,
    "JVaultClaim": JVaultClaim,
}


def register_jvault(reg: Registries) -> Registries:
    """Register everything specs/jvault.mch needs into `reg`."""
    reg.block_types.update(BLOCK_TYPES)
    reg.message_types.update(MESSAGE_TYPES)

    @reg.register_lookup("jvault_assets")
    async def jvault_assets(stake_wallet):
        # G10 record lookup: resolve the stake wallet to its (staking_pool,
        # asset, jvault_asset) triple as one record, or null when unresolvable.
        # Mirrors extract_jvault_assets' (None, None, None) total-failure sentinel
        # as a null record; a partial resolution keeps null-valued fields so the
        # build can reject on `assets.asset == null` exactly where the cores do.
        pool, asset, jvault_asset = await extract_jvault_assets(stake_wallet)
        if pool is None and asset is None and jvault_asset is None:
            return None
        return {"staking_pool": pool, "asset": asset, "jvault_asset": jvault_asset}

    @reg.register_fn("jvault_transfer_amount")
    async def jvault_transfer_amount(transfer_block):
        # The outer transfer amount as a raw int (legacy reads
        # block.jetton_transfer_message.amount for both stake's staked_amount and
        # claim's claimed_amounts). The composed jetton_transfer's
        # `.jetton_transfer_message` attribute is invisible to the expression
        # layer (only msg/body/data), and re-`parse`ing its get_body does NOT
        # round-trip (amount comes back null), so this stays a host read.
        return transfer_block.jetton_transfer_message.amount

    @reg.register_fn("jvault_stake_period")
    async def jvault_stake_period(transfer_block):
        # blocks/jvault.py build_jvault_stake_core: skip the 32-bit op, read the
        # 32-bit period out of the transfer forward_payload sub-cell (the
        # sanctioned irreducible cell parse). Reads the block's already-parsed
        # message, the exact object the legacy core uses.
        forward_payload = transfer_block.jetton_transfer_message.forward_payload
        body = Cell.from_boc(forward_payload)[0].begin_parse()
        body.load_uint(32)
        return body.load_uint(32)

    @reg.register_fn("jvault_account_list")
    async def jvault_account_list(addresses):
        return [AccountId(a) for a in addresses]

    return reg


def jvault_registries() -> Registries:
    """Fresh Registries ready to compile specs/jvault.mch."""
    return register_jvault(Registries())
