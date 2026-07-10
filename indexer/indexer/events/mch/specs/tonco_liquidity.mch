# Tonco deposit liquidity — ported from blocks/liquidity.py
# ToncoDepositLiquidityMatcher.
#
# Like the legacy matcher (which runs after the jetton/nft matchers in
# event_processing), the pattern relies on prebuilt `btype jetton_transfer`
# and `btype nft_mint` blocks (prerequisites: NftMintBlockMatcher,
# JettonTransferBlockMatcher, PTonTransferMatcher).
#
# Shape notes:
# - the anchor's optional MinAndRefund child chain is wrapped in a one-item
#   children set so the parent edge stays attached to the anchor (§6.5 CB
#   exemption; a bare `-> ... <-` chain is C009) — dedust_swap precedent;
# - each refund transfer carries `-> { maybe op PTonTransfer }`: legacy
#   output_ton_or_jetton has an optional PTonTransfer child_matcher;
# - `maybe op ExcessMessage` mirrors the legacy explicit children item — the
#   engine would absorb it as an excess sibling anyway, kept for fidelity;
# - the input alternative keeps legacy OrMatcher order: pton arm (JettonNotify
#   fed by a raw PTonTransfer call) first, plain jetton transfer second.
#
# HYBRID declarative build. The block assembly is declarative; the imperative
# residue (input-transfer resolution via previous_block, per-side BOC parsing
# of the AddLiquidity / NftPositionInit / JettonNotify forward payload, the two
# jetton-wallet interface lookups + pton-master asset classification, the
# amount1/amount2 ordering by which side was deposited, and the excess-payout
# pton navigation) stays in one host fn returning the field record — the
# ToncoDepositLiquidityData the shared core already builds. The `out` block
# flattens that record into the produced block's data map. The fn returns null
# to reject (spec §10.7).
#
# Product is a generic Block(btype tonco_deposit_liquidity) with a dict data
# map (the legacy block carried a ToncoDepositLiquidityData dataclass); the
# serializer's _fill reads both via _dget.

opcode ToncoAccountV3AddLiquidity = 0x3ebe5431
opcode ToncoPoolV3FundAccount     = 0x4468de77
opcode ToncoPoolV3MinAndRefund    = 0x81702ef8
opcode ToncoRouterV3PayTo         = 0xa1daa96d
opcode JettonNotify               = 0x7362d09c
opcode PTonTransfer               = 0x01f3835d
opcode ExcessMessage              = 0xd53276db

matcher tonco_deposit_liquidity {
    produces tonco_deposit_liquidity
    entry    @add_liquidity

    op ToncoAccountV3AddLiquidity @add_liquidity
      -> { maybe op ToncoPoolV3MinAndRefund
             -> { maybe op ToncoRouterV3PayTo @refund_router_call
                    -> { maybe btype jetton_transfer @excess_transfer_0
                           -> { maybe op PTonTransfer }
                       ; maybe btype jetton_transfer @excess_transfer_1
                           -> { maybe op PTonTransfer } }
                ; btype nft_mint @nft_mint
                ; maybe op ExcessMessage } }
      <- op ToncoPoolV3FundAccount
      <- ( op JettonNotify @ton_input <- op PTonTransfer @ton_input_addition
         | btype jetton_transfer @jetton_input )

    let r = tonco_deposit_liquidity_data(consumed)
    reject when r == null
    out {
        sender:            r.sender
        pool:              r.pool
        account_contract:  r.account_contract
        position_amount_1: r.position_amount_1
        position_amount_2: r.position_amount_2
        lp_tokens_minted:  r.lp_tokens_minted
        tick_lower:        r.tick_lower
        tick_upper:        r.tick_upper
        nft_index:         r.nft_index
        nft_address:       r.nft_address
        amount_1:          r.amount_1
        asset_1:           r.asset_1
        sender_wallet_1:   r.sender_wallet_1
        amount_2:          r.amount_2
        asset_2:           r.asset_2
        sender_wallet_2:   r.sender_wallet_2
        excesses:          r.excesses
    }
}
