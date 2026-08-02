"""MCH registrations for the JVault family in specs/jvault.mch.

specs/jvault.mch uses a declarative build: the runtime evaluates
parse/lookup/reject/failed/out field-for-field with the legacy cores in
blocks/jvault.py, so this module registers only the produced block types, the
message parsers (`parse`), the record-returning assets lookup, and the two
host functions the language cannot express.

Host hooks and the language feature each one supplies:

- lookup `jvault_assets` returns a record from extract_jvault_assets
  (blocks/jvault.py). It performs two chained interface-repository reads plus a
  data-cell parse and returns one record {staking_pool, asset, jvault_asset} or
  null. The build reads fields such as `assets.asset` and rejects on null.
- fn `jvault_stake_period`: parses the stake period out of the jetton-transfer
  forward_payload sub-cell. Protocol-specific cell parsing is not a data field
  an inline test can reach. It takes the composed transfer's
  `data.forward_payload` (a b64 BOC string both
  engines carry), not the legacy `.jetton_transfer_message` attribute, so it is
  a pure function over a string and portable to the C++ engine.
The claim body's jetton-wallet address list maps through the AccountId
constructor with `map(xs as e => account(e))` directly in the spec.

The transfer amount is read directly from
`transfer.data.desired_amount.value` (stake) or
`withdraw.data.desired_amount.value` (claim).

The legacy JVault*BlockMatcher and the build_*_core functions stay the parity
reference (blocks/jvault.py).
"""
from __future__ import annotations

import base64

from pytoniq_core import Builder, Cell, Slice

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
}


class JVaultClaimTwin:
    """ABI-shaped twin whose map is a list of ``[key, value]`` pairs."""

    opcode = JVaultClaim.opcode

    def __init__(self, slice: Slice):
        slice.load_uint(32)  # opcode (checked by the matcher's `op`)
        self.query_id = slice.load_uint(64)
        entries = slice.load_dict(
            267,
            key_deserializer=lambda key: (
                Builder().store_bits(key).end_cell().to_slice().load_address()
            ),
            value_deserializer=lambda remaining: remaining,
        ) or {}
        self.jettons_to_claim = [
            [address, remaining] for address, remaining in entries.items()
        ]


MESSAGE_TYPES["JVaultClaim"] = JVaultClaimTwin


def register_jvault(reg: Registries) -> Registries:
    """Register everything specs/jvault.mch needs into `reg`."""
    reg.block_types.update(BLOCK_TYPES)
    reg.message_types.update(MESSAGE_TYPES)

    @reg.register_lookup("jvault_assets")
    async def jvault_assets(stake_wallet):
        # Resolve the stake wallet to its (staking_pool,
        # asset, jvault_asset) triple as one record, or null when unresolvable.
        # Mirrors extract_jvault_assets' (None, None, None) total-failure sentinel
        # as a null record; a partial resolution keeps null-valued fields so the
        # build can reject on `assets.asset == null` exactly where the cores do.
        pool, asset, jvault_asset = await extract_jvault_assets(stake_wallet)
        if pool is None and asset is None and jvault_asset is None:
            return None
        return {"staking_pool": pool, "asset": asset, "jvault_asset": jvault_asset}

    @reg.register_fn("jvault_stake_period")
    async def jvault_stake_period(forward_payload_b64):
        # blocks/jvault.py build_jvault_stake_core: skip the 32-bit op, read the
        # 32-bit period out of the transfer forward_payload sub-cell (the
        # sanctioned irreducible cell parse). Takes the composed jetton_transfer's
        # `data.forward_payload` — a b64 BOC string both the legacy and
        # declarative jetton builds emit identically — so the fn is a pure
        # string->int transform with a direct C++ analog (no block-internal
        # reach). None forward_payload (absent) yields a null period.
        if forward_payload_b64 is None:
            return None
        body = Cell.from_boc(base64.b64decode(forward_payload_b64))[0].begin_parse()
        body.load_uint(32)
        return body.load_uint(32)

    return reg


def jvault_registries() -> Registries:
    """Fresh Registries ready to compile specs/jvault.mch."""
    return register_jvault(Registries())
