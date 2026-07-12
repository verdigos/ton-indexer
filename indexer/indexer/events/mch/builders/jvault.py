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
  irreducible escape (§15b), not a data field an inline test could reach. It
  takes the composed transfer's `data.forward_payload` (a b64 BOC string both
  engines carry), NOT the legacy `.jetton_transfer_message` attribute, so it is
  a pure fn over a string and portable to the C++ engine (F1).
The former `jvault_account_list` fn is gone (W6): the claim body's jetton-wallet
address list now maps through the AccountId constructor with the G-LC
comprehension `map(xs as e => account(e))` directly in the spec — the fn existed
only because element-wise constructor-map was deferred (§16.1).

The old `jvault_transfer_amount` fn is gone: F1+F2 read the transfer amount
straight off `transfer.data.desired_amount.value` (stake) /
`withdraw.data.desired_amount.value` (claim).

The legacy JVault*BlockMatcher and the build_*_core functions stay the parity
reference (blocks/jvault.py).
"""
from __future__ import annotations

import base64

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
