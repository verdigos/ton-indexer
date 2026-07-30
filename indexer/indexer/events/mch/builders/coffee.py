"""mch host fn for the coffee swap matcher (specs/coffee_swap.mch).

specs/coffee_swap.mch is a HYBRID declarative build: the pattern, reject/failed
wiring and out-field mapping live in the .mch, and the imperative residue is
the single host fn below. The shared core (blocks/swaps.py
build_coffee_swap_core) needs the incoming transfer, the CoffeePayoutInternal
and the outgoing transfer, plus the full consumed set for find_call_contracts.
The legacy matcher extracts the three via labels; the mch engine has no labels,
and this host fn (which predates the T18 capture fixes — exit-branch and
post-recursion captures bind since then, spec §7.2) derives them structurally
from the consumed set, which it needs anyway for the core's scan:

- in_transfer: the anchor's previous_block (the anchor's parent alternative is
  required, so the block is always consumed when the match succeeds);
- payout: the single CoffeePayoutInternal in the consumed set (the recursion
  exit fires exactly once per match);
- out_transfer: the consumed child of the payout, first in consumed order —
  matching legacy get_labeled semantics when several payout children matched.

Returns None to reject the match (spec §10.7).
"""
from __future__ import annotations

from types import SimpleNamespace

from pytoniq_core import Slice

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block
from indexer.events.blocks.liquidity import CoffeeCreateVaultBlock
from indexer.events.blocks.messages.coffee import (
    CoffeeCreateVault,
    CoffeeDeploy,
    CoffeeDepositLiquidityInternal,
    CoffeeDepositLiquidityNative,
    CoffeeDepositLiquiditySuccessfulEvent,
    CoffeeLiquidityWithdrawalEvent,
    CoffeeNotification,
    CoffeePayout,
    CoffeePayoutInternal,
    CoffeeSwapInternal,
    CoffeeSwapNative,
    CoffeeSwapSuccessfulEvent,
    DepositLiquidityParamsTrimmed,
    PoolParams,
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
    reject (core returns None on a missing role or no hops, spec §10.7)."""
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


class CoffeeCreateLiquidityDepositoryRequest:
    """create_liquidity_depository_request#c0ffee27 — FLAT twin of the
    mch-msgs.schema row of the same name.

    The legacy blocks/messages/coffee.py class of this name exposes the pool
    assets NESTED (`.pool_params.first`), but the C++ schema generator FLATTENS a
    `ref { … }` group's subfields into the parent object, so the two hosts would
    disagree on the field path. This twin mirrors the schema exactly — flat
    `pool_first`/`pool_second`, everything the spec does not read parsed and
    dropped — the same convention as builders/dedust_v2.py's message twins.
    """

    opcode = 0xC0FFEE27

    def __init__(self, slice: Slice):
        slice.load_uint(32)  # opcode (checked by the matcher's `op`)
        slice.load_uint(64)  # query_id, parsed-and-dropped
        self.amount = slice.load_coins()
        DepositLiquidityParamsTrimmed(slice.load_ref().to_slice())  # dropped
        pool_params = PoolParams(slice.load_ref().to_slice())
        self.pool_first = pool_params.first
        self.pool_second = pool_params.second
        self.sender = slice.load_address()
        slice.load_ref()  # proof, parsed-and-dropped


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
    "CoffeeCreateVault":                      CoffeeCreateVault,
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
