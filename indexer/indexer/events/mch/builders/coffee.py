"""mch host fn for the coffee swap matcher (specs/coffee_swap.mch).

specs/coffee_swap.mch is a HYBRID declarative build: the pattern, reject/failed
wiring and out-field mapping live in the .mch, and the imperative residue is
the single host fn below. The shared core (blocks/swaps.py
build_coffee_swap_core) needs the incoming transfer, the CoffeePayoutInternal
and the outgoing transfer, plus the full consumed set for find_call_contracts.
Exit-branch and post-recursion captures bind. This host fn deliberately derives
the three roles structurally from the consumed set because the shared legacy
core needs that full set for its call-contract scan:

- in_transfer: the anchor's previous_block (the anchor's parent alternative is
  required, so the block is always consumed when the match succeeds);
- payout: the single CoffeePayoutInternal in the consumed set (the recursion
  exit fires exactly once per match);
- out_transfer: the consumed child of the payout, first in consumed order —
  matching legacy get_labeled semantics when several payout children matched.

Returns None to reject the match.
"""
from __future__ import annotations

from types import SimpleNamespace

from pytoniq_core import Builder, Slice

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block
from indexer.events.blocks.liquidity import CoffeeCreateVaultBlock
from indexer.events.blocks.messages.coffee import (
    CoffeeCreatePoolRequest,
    CoffeeCreateVault,
    CoffeeDeploy,
    CoffeeDepositLiquidityInternal,
    CoffeeDepositLiquidityNative,
    CoffeeDepositLiquiditySuccessfulEvent,
    CoffeeLiquidityWithdrawalEvent,
    CoffeeNotification,
    CoffeePayout,
    CoffeePayoutInternal,
    CoffeeStakingClaimRewards,
    CoffeeStakingPositionWithdraw2,
    CoffeeStakingPositionWithdraw3,
    CoffeeSwapInternal,
    CoffeeSwapNative,
    CoffeeSwapSuccessfulEvent,
    DepositLiquidityParamsTrimmed,
    PoolCreationParams,
)
from indexer.events.blocks.messages.jettons import JettonInternalTransfer
from indexer.events.blocks.messages import ExcessMessage
from indexer.events.blocks.swaps import JettonSwapBlock, build_coffee_swap_core
from indexer.events.mch.registry import Registries

OPCODES = {
    "CoffeeSwapNative":          CoffeeSwapNative.opcode,
    "CoffeeSwapInternal":        CoffeeSwapInternal.opcode,
    "CoffeeSwapSuccessfulEvent": CoffeeSwapSuccessfulEvent.opcode,
    "CoffeePayoutInternal":      CoffeePayoutInternal.opcode,
    "CoffeePayout":              CoffeePayout.opcode,
    "CoffeeNotification":        CoffeeNotification.opcode,
}

BLOCK_TYPES = {
    "JettonSwapBlock": JettonSwapBlock,
}

_OUT_OPCODES = (CoffeePayout.opcode, CoffeeNotification.opcode)


async def _coffee_swap_data(consumed):
    """Imperative residue of the coffee swap build: derive the three leg roles
    from the consumed set, delegate to the shared core, and expose its data
    dict as a namespace (`.failed` drives the failed flag). Returns None to
    reject (the core returns None on a missing role or no hops)."""
    block, others = consumed[0], list(consumed[1:])

    in_transfer = None
    prev = block.previous_block
    if prev is not None and prev in others:
        if prev.btype == "jetton_transfer" or (
            isinstance(prev, CallContractBlock)
            and prev.opcode == CoffeeSwapNative.opcode
        ):
            in_transfer = prev

    payout = next(
        (
            b for b in others
            if isinstance(b, CallContractBlock)
            and b.opcode == CoffeePayoutInternal.opcode
        ),
        None,
    )

    out_transfer = None
    if payout is not None:
        for b in others:
            if b.previous_block is payout and (
                b.btype == "jetton_transfer"
                or (isinstance(b, CallContractBlock) and b.opcode in _OUT_OPCODES)
            ):
                out_transfer = b
                break

    new_block = await build_coffee_swap_core(
        block, others, in_transfer, payout, out_transfer
    )
    if new_block is None:
        return None
    return SimpleNamespace(failed=new_block.failed, **new_block.data)


def register_coffee(reg: Registries) -> Registries:
    """Register everything specs/coffee_swap.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)
    reg.register_fn("coffee_swap_data")(_coffee_swap_data)
    return reg


def coffee_registries() -> Registries:
    """Fresh Registries ready to compile specs/coffee_swap.mch."""
    return register_coffee(Registries())


# --- coffee liquidity/vault (specs/coffee_liquidity.mch) ---------------------


def _load_coffee_asset_abi(slice: Slice) -> SimpleNamespace:
    """Read Coffee's Asset union without converting it to the legacy Asset."""
    tag = slice.load_uint(2)
    if tag == 0b00:
        return SimpleNamespace(**{"$": "AssetTon"})
    if tag == 0b01:
        workchain = slice.load_uint(8)
        hash_cell = Builder().store_bytes(slice.load_bytes(32)).end_cell()
        return SimpleNamespace(
            **{"$": "AssetJetton", "workchain": workchain, "hash": hash_cell}
        )
    raise ValueError(f"Invalid Coffee Asset tag: {tag}")


def _load_coffee_pool_params_abi(slice: Slice) -> SimpleNamespace:
    pool_first = _load_coffee_asset_abi(slice)
    pool_second = _load_coffee_asset_abi(slice)
    slice.load_uint(3)  # amm, parsed-and-dropped
    slice.load_maybe_ref()  # amm_settings, parsed-and-dropped
    return SimpleNamespace(pool_first=pool_first, pool_second=pool_second)


class CoffeeCreateLiquidityDepositoryRequest:
    """ABI-shaped twin of create_liquidity_depository_request#c0ffee27."""

    opcode = 0xC0FFEE27

    def __init__(self, slice: Slice):
        slice.load_uint(32)  # opcode (checked by the matcher's `op`)
        slice.load_uint(64)  # query_id, parsed-and-dropped
        self.amount = slice.load_coins()
        DepositLiquidityParamsTrimmed(slice.load_ref().to_slice())  # dropped
        pool_params = _load_coffee_pool_params_abi(slice.load_ref().to_slice())
        self.pool_params = SimpleNamespace(ref=pool_params)
        self.sender = slice.load_address()
        slice.load_ref()  # proof, parsed-and-dropped


class CoffeeCreateVaultTwin:
    """ABI-shaped twin that preserves the raw Asset union for asset_of()."""

    opcode = CoffeeCreateVault.opcode

    def __init__(self, slice: Slice):
        slice.load_uint(32)  # opcode (checked by the matcher's `op`)
        self.query_id = slice.load_uint(64)
        self.asset = _load_coffee_asset_abi(slice)


LIQUIDITY_OPCODES = {
    "CoffeeDepositLiquidityNative":           CoffeeDepositLiquidityNative.opcode,
    "CoffeeCreateVault":                      CoffeeCreateVault.opcode,
    "CoffeePayoutInternal":                   CoffeePayoutInternal.opcode,
    "CoffeeDeploy":                           CoffeeDeploy.opcode,
    "CoffeeCreateLiquidityDepositoryRequest": CoffeeCreateLiquidityDepositoryRequest.opcode,
    "CoffeeDepositLiquidityInternal":         CoffeeDepositLiquidityInternal.opcode,
    "CoffeePayout":                           CoffeePayout.opcode,
    "CoffeeDepositLiquiditySuccessfulEvent":  CoffeeDepositLiquiditySuccessfulEvent.opcode,
    "CoffeeNotification":                     CoffeeNotification.opcode,
    "JettonInternalTransfer":                 JettonInternalTransfer.opcode,
    "ExcessMessage":                          ExcessMessage.opcode,
    # TEP-89 jetton-wallet discovery pair — raw literals in the legacy matcher.
    "ProvideWalletAddress": 0x2C76B973,
    "TakeWalletAddress":    0xD1735400,
}

LIQUIDITY_BLOCK_TYPES = {
    "CoffeeCreateVaultBlock": CoffeeCreateVaultBlock,
    # The deposit matcher produces the canonical generic Block, exactly like the
    # legacy one (`Block("coffee_deposit_liquidity", [], data.__dict__)`).
    "coffee_deposit_liquidity": Block,
}

LIQUIDITY_MESSAGE_TYPES = {
    "CoffeeCreateVault":                      CoffeeCreateVaultTwin,
    "CoffeeCreateLiquidityDepositoryRequest": CoffeeCreateLiquidityDepositoryRequest,
    # Already FLAT (lp_amount at the top level), so the legacy class doubles as
    # its own schema twin.
    "CoffeeDepositLiquiditySuccessfulEvent":  CoffeeDepositLiquiditySuccessfulEvent,
}


def register_coffee_liquidity(reg: Registries) -> Registries:
    """Register everything specs/coffee_liquidity.mch needs into `reg`.

    Both matchers in that spec are FULLY DECLARATIVE — zero host fns, zero
    lookups — so this registers only names: opcodes, produced btypes and the
    message parsers the `parse` statements need.
    """
    reg.opcodes.update(LIQUIDITY_OPCODES)
    reg.block_types.update(LIQUIDITY_BLOCK_TYPES)
    reg.message_types.update(LIQUIDITY_MESSAGE_TYPES)
    return reg


def coffee_liquidity_registries() -> Registries:
    """Fresh Registries ready to compile specs/coffee_liquidity.mch."""
    return register_coffee_liquidity(Registries())


# --- coffee withdraw liquidity (specs/coffee_withdraw.mch) -------------------

WITHDRAW_OPCODES = {
    "CoffeePayoutInternal":           CoffeePayoutInternal.opcode,
    "CoffeePayout":                   CoffeePayout.opcode,
    "CoffeeLiquidityWithdrawalEvent": CoffeeLiquidityWithdrawalEvent.opcode,
    "CoffeeNotification":             CoffeeNotification.opcode,
}

WITHDRAW_BLOCK_TYPES = {
    "dex_withdraw_liquidity": Block,
}

WITHDRAW_MESSAGE_TYPES = {
    # Already FLAT (amount1/amount2 at the top level) — its own schema twin.
    "CoffeeLiquidityWithdrawalEvent": CoffeeLiquidityWithdrawalEvent,
}


def register_coffee_withdraw(reg: Registries) -> Registries:
    """Register everything specs/coffee_withdraw.mch needs into `reg`.

    FULLY DECLARATIVE, like the rest of the coffee liquidity family: names only.
    """
    reg.opcodes.update(WITHDRAW_OPCODES)
    reg.block_types.update(WITHDRAW_BLOCK_TYPES)
    reg.message_types.update(WITHDRAW_MESSAGE_TYPES)
    return reg


def coffee_withdraw_registries() -> Registries:
    """Fresh Registries ready to compile specs/coffee_withdraw.mch."""
    return register_coffee_withdraw(Registries())


# --- coffee MEV protect (specs/coffee_mev.mch) -------------------------------

MEV_BLOCK_TYPES = {
    # Both arms produce the canonical generic Block, exactly like the legacy
    # matcher (`Block("coffee_mev_protect_hold_funds", [], {...})`).
    "coffee_mev_protect_hold_funds": Block,
}


def register_coffee_mev(reg: Registries) -> Registries:
    """Register everything specs/coffee_mev.mch needs into `reg`.

    FULLY DECLARATIVE — no host fns, no predicates, no `parse` (neither arm
    reads a message body), so this is the produced btype and nothing else.
    """
    reg.block_types.update(MEV_BLOCK_TYPES)
    return reg


def coffee_mev_registries() -> Registries:
    """Fresh Registries ready to compile specs/coffee_mev.mch."""
    return register_coffee_mev(Registries())


# --- coffee staking (specs/coffee_staking.mch) -------------------------------

STAKING_BLOCK_TYPES = {
    "coffee_staking_deposit": Block,
    "coffee_staking_withdraw": Block,
    "coffee_staking_claim_rewards": Block,
}

STAKING_MESSAGE_TYPES = {
    # All three legacy classes are already FLAT in the fields the spec reads, so
    # each doubles as its own protocol ABI twin. CoffeeStakingPositionWithdraw2
    # additionally flattens its mandatory `jetton_data` ref into
    # jetton_wallet/jetton_amount, which the schema row does not walk at all —
    # harmless, the spec reads neither from withdraw_2.
    "CoffeeStakingClaimRewards": CoffeeStakingClaimRewards,
    "CoffeeStakingPositionWithdraw2": CoffeeStakingPositionWithdraw2,
    "CoffeeStakingPositionWithdraw3": CoffeeStakingPositionWithdraw3,
}


def register_coffee_staking(reg: Registries) -> Registries:
    """Register everything specs/coffee_staking.mch needs into `reg`."""
    reg.block_types.update(STAKING_BLOCK_TYPES)
    reg.message_types.update(STAKING_MESSAGE_TYPES)
    return reg


def coffee_staking_registries() -> Registries:
    """Fresh Registries ready to compile specs/coffee_staking.mch."""
    return register_coffee_staking(Registries())


# --- coffee pool creation (specs/coffee_create_pool.mch) ---------------------


class CoffeeCreatePoolCreatorRequestTwin:
    """ABI-shaped twin of create_pool_creator_request#c0ffee23.

    Two reasons this cannot be the legacy class of that name
    (messages/coffee.py:713):

    1. That class is BROKEN. The wire is
       `… params:PoolParams creation_params:^PoolCreationParams sender:… proof:^Cell`
       but it parses `creation_params` INLINE, so it runs off the end of a real
       body (IndexError on the first bit past PoolParams). Nothing calls it —
       CoffeeCreatePoolCreatorMatcher only reads its `.opcode`, and the legacy
       build parses the LEG instead — which is why the bug never surfaced.
    2. Even fixed, it would eagerly convert its assets to legacy Asset values
       instead of preserving the ABI union objects consumed by asset_of().
    """

    opcode = 0xC0FFEE23

    def __init__(self, slice: Slice):
        slice.load_uint(32)  # opcode (checked by the matcher's `op`)
        slice.load_uint(64)  # query_id, parsed-and-dropped
        self.amount = slice.load_coins()
        pool_params = _load_coffee_pool_params_abi(slice)  # INLINE, not a ref
        self.pool_first = pool_params.pool_first
        self.pool_second = pool_params.pool_second
        creation_params = PoolCreationParams(slice.load_ref().to_slice())
        self.creation_params = SimpleNamespace(
            ref=SimpleNamespace(recipient=creation_params.public.recipient)
        )
        self.sender = slice.load_address()
        slice.load_ref()  # proof, parsed-and-dropped


CREATE_POOL_BLOCK_TYPES = {
    "coffee_create_pool_creator": Block,
    "coffee_create_pool": Block,
}

CREATE_POOL_MESSAGE_TYPES = {
    "CoffeeCreatePoolCreatorRequest": CoffeeCreatePoolCreatorRequestTwin,
    # Already flat in amount1/amount2/tx_initiator — its own schema twin.
    "CoffeeCreatePoolRequest": CoffeeCreatePoolRequest,
    "CoffeeDepositLiquiditySuccessfulEvent": CoffeeDepositLiquiditySuccessfulEvent,
}


def register_coffee_create_pool(reg: Registries) -> Registries:
    """Register everything specs/coffee_create_pool.mch needs into `reg`.

    Both matchers are FULLY DECLARATIVE; this registers names only.
    """
    reg.block_types.update(CREATE_POOL_BLOCK_TYPES)
    reg.message_types.update(CREATE_POOL_MESSAGE_TYPES)
    return reg


def coffee_create_pool_registries() -> Registries:
    """Fresh Registries ready to compile specs/coffee_create_pool.mch."""
    return register_coffee_create_pool(Registries())
