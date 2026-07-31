"""mch builder for the tonco deposit-liquidity matcher (specs/tonco_liquidity.mch).

The legacy build (blocks/liquidity.py ToncoDepositLiquidityMatcher.build_block)
works off LabelBlocks, which the mch engine does not emit. This builder derives
the label roles structurally from `Match._mch_consumed` + tree navigation (the
tree is intact until the engine merges) and delegates to the shared core
`build_tonco_deposit_liquidity_core`:

- input_transfer: the FundAccount call's previous_block — the parent
  alternative is required, so that block is always consumed: the prebuilt
  jetton_transfer block, or the raw JettonNotify call on the pton arm;
- nft_mint: the single consumed nft_mint block;
- excess transfers: consumed jetton_transfer blocks hanging off the consumed
  ToncoRouterV3PayTo call, in consumption (= children-set) order, padded with
  None to the legacy fixed arity of two.

The legacy additional_blocks bookkeeping (the raw PTonTransfer feeding the
notify) is unnecessary here: the pattern consumes that call, so the synthesized
merge covers it. Returns None to reject the match (spec §10.7).
"""
from __future__ import annotations

from types import SimpleNamespace

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block
from indexer.events.blocks.liquidity import (
    build_tonco_deposit_liquidity_core,
    resolve_tonco_withdraw_payouts,
)
from indexer.events.blocks.messages import (
    ExcessMessage,
    PTonTransfer,
    ToncoAccountV3AddLiquidity,
    ToncoPoolV3Burn,
    ToncoPoolV3FundAccount,
    ToncoPoolV3MinAndRefund,
    ToncoPoolV3StartBurn,
    ToncoPositionNftV3PositionBurn,
    ToncoRouterV3PayTo,
)
from indexer.events.blocks.messages.jettons import JettonNotify
from indexer.events.mch.registry import Registries

OPCODES = {
    "ToncoAccountV3AddLiquidity":     ToncoAccountV3AddLiquidity.opcode,
    "ToncoPoolV3FundAccount":         ToncoPoolV3FundAccount.opcode,
    "ToncoPoolV3MinAndRefund":        ToncoPoolV3MinAndRefund.opcode,
    "ToncoRouterV3PayTo":             ToncoRouterV3PayTo.opcode,
    "ToncoPoolV3StartBurn":           ToncoPoolV3StartBurn.opcode,
    "ToncoPositionNftV3PositionBurn": ToncoPositionNftV3PositionBurn.opcode,
    "ToncoPoolV3Burn":                ToncoPoolV3Burn.opcode,
    "JettonNotify":                   JettonNotify.opcode,
    "PTonTransfer":                   PTonTransfer.opcode,
    "ExcessMessage":                  ExcessMessage.opcode,
}

# The withdraw chain's three `parse` targets. Each legacy class is an EXACT twin
# of its mch-msgs.schema entry (whole body parsed, plain ints throughout), so no
# purpose-built twin class is needed.
MESSAGE_TYPES = {
    "ToncoPoolV3StartBurn":           ToncoPoolV3StartBurn,
    "ToncoPositionNftV3PositionBurn": ToncoPositionNftV3PositionBurn,
    "ToncoPoolV3Burn":                ToncoPoolV3Burn,
}

BLOCK_TYPES = {
    "tonco_deposit_liquidity": Block,
    "tonco_withdraw_liquidity": Block,
}


async def _tonco_deposit_liquidity_data(consumed: list[Block]):
    """Host residue of the tonco deposit-liquidity build (hybrid declarative
    spec). Derives the legacy label roles structurally from the consumed set +
    previous_block navigation and delegates to
    build_tonco_deposit_liquidity_core; returns the ToncoDepositLiquidityData
    record the `out` block flattens, or None to reject (spec §10.7)."""
    block = consumed[0]
    try:
        # anchor <- FundAccount <- input transfer
        input_transfer = block.previous_block.previous_block
        nft_mint = next((b for b in consumed if b.btype == "nft_mint"), None)
        pay_to = next((b for b in consumed
                       if isinstance(b, CallContractBlock)
                       and b.opcode == ToncoRouterV3PayTo.opcode), None)
        excess_transfers: list = []
        if pay_to is not None:
            excess_transfers = [b for b in consumed
                                if b.btype == "jetton_transfer"
                                and b.previous_block is pay_to][:2]
        while len(excess_transfers) < 2:
            excess_transfers.append(None)
        blocks = await build_tonco_deposit_liquidity_core(
            block, nft_mint, input_transfer, excess_transfers)
    except Exception:
        return None
    if not blocks:
        return None
    return blocks[0].data


def _payout_slot(payouts: list[dict], i: int, key: str):
    """`processed_payouts[i].get(key) if len(...) > i else None` — the legacy
    guard, kept verbatim even though the core always returns exactly two legs."""
    return payouts[i].get(key) if len(payouts) > i else None


async def _tonco_withdraw_payouts(consumed: list[Block]):
    """Host residue of the tonco withdraw-liquidity build (hybrid declarative
    spec) — the payout half only; the spec computes the scalar half from its
    three `parse`d bodies.

    Derives the legacy label roles structurally from `Match._mch_consumed` (the
    tree is intact until the engine merges): the router PayTo call, then the up-
    to-two jetton_transfer legs hanging off it in consumption order, padded to
    the legacy fixed arity of two. Delegates to the shared core
    resolve_tonco_withdraw_payouts, which is also what the legacy matcher calls.

    Returns the record the `out` block reads, or None to reject (spec §10.7).
    Note the legacy build lets a malformed PayTo body raise out of build_block
    (the trace then falls back to basic actions); here it rejects instead, so
    the rest of the trace still classifies."""
    try:
        pay_to = next((b for b in consumed
                       if isinstance(b, CallContractBlock)
                       and b.opcode == ToncoRouterV3PayTo.opcode), None)
        if pay_to is None:
            return None
        legs: list = [b for b in consumed
                      if b.btype == "jetton_transfer"
                      and b.previous_block is pay_to][:2]
        while len(legs) < 2:
            legs.append(None)
        payouts, _extra_blocks, failed = await resolve_tonco_withdraw_payouts(
            pay_to, legs[0], legs[1])
    except Exception:
        return None
    if payouts is None:
        return None
    return SimpleNamespace(
        amount1_out=_payout_slot(payouts, 0, "amount"),
        asset1_out=_payout_slot(payouts, 0, "asset"),
        dex_wallet_1=_payout_slot(payouts, 0, "dex_wallet"),
        dex_jetton_wallet_1=_payout_slot(payouts, 0, "dex_jetton_wallet"),
        wallet1=_payout_slot(payouts, 0, "wallet"),
        amount2_out=_payout_slot(payouts, 1, "amount"),
        asset2_out=_payout_slot(payouts, 1, "asset"),
        dex_wallet_2=_payout_slot(payouts, 1, "dex_wallet"),
        dex_jetton_wallet_2=_payout_slot(payouts, 1, "dex_jetton_wallet"),
        wallet2=_payout_slot(payouts, 1, "wallet"),
        failed=failed,
    )


def register_tonco_liquidity(reg: Registries) -> Registries:
    """Register everything specs/tonco_liquidity.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)
    reg.message_types.update(MESSAGE_TYPES)
    reg.register_fn("tonco_deposit_liquidity_data")(_tonco_deposit_liquidity_data)
    reg.register_fn("tonco_withdraw_payouts")(_tonco_withdraw_payouts)
    return reg


def tonco_liquidity_registries() -> Registries:
    """Fresh Registries ready to compile specs/tonco_liquidity.mch."""
    return register_tonco_liquidity(Registries())
