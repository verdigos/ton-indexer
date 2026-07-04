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
# The builder is consumed-set/tree-driven (Match._mch_consumed +
# previous_block navigation) delegating to build_tonco_deposit_liquidity_core;
# captures are kept for pattern fidelity only.

opcode ToncoAccountV3AddLiquidity = 0x3ebe5431
opcode ToncoPoolV3FundAccount     = 0x4468de77
opcode ToncoPoolV3MinAndRefund    = 0x81702ef8
opcode ToncoRouterV3PayTo         = 0xa1daa96d
opcode JettonNotify               = 0x7362d09c
opcode PTonTransfer               = 0x01f3835d
opcode ExcessMessage              = 0xd53276db

matcher tonco_deposit_liquidity {
    produces ToncoDepositLiquidityBlock
    build    build_tonco_deposit_liquidity
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
}
