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

opcode ToncoAccountV3AddLiquidity      = 0x3ebe5431
opcode ToncoPoolV3FundAccount          = 0x4468de77
opcode ToncoPoolV3MinAndRefund         = 0x81702ef8
opcode ToncoRouterV3PayTo              = 0xa1daa96d
opcode ToncoPoolV3StartBurn            = 0x530b5f2c
opcode ToncoPositionNftV3PositionBurn  = 0x46ca335a
opcode ToncoPoolV3Burn                 = 0xd73ac09d
opcode JettonNotify                    = 0x7362d09c
opcode PTonTransfer                    = 0x01f3835d
opcode ExcessMessage                   = 0xd53276db

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


# Tonco withdraw liquidity — ported from blocks/liquidity.py
# ToncoWithdrawLiquidityMatcher (the third tonco liquidity matcher; the deploy-pool
# one is not ported).
#
# Chain, straight off the legacy nested ContractMatchers:
#   StartBurn 0x530b5f2c (user -> Pool)                       <- anchor
#     PositionBurn 0x46ca335a (Pool -> position NFT)          # names the owner
#       PoolV3Burn 0xd73ac09d (NFT -> Pool)                   # authoritative index
#         RouterV3PayTo 0xa1daa96d (Pool -> Router)           # declares both slots
#           payout leg 1 (optional jetton_transfer -> optional PTonTransfer)
#           payout leg 2 (same)
#
# HYBRID declarative build, split along what the DSL can actually express.
#
# DECLARATIVE (the scalar half): sender / pool / burned-nft index+address /
# liquidity_burnt / tick range all come off the three `parse`d bodies. These are
# the FIRST users of the schema's uint128 and int24 leaves (mch-msgs.schema) —
# tick indices are signed and go negative, share amounts overflow 64 bits, so
# before this port neither field was reachable. All three messages reuse their
# LEGACY class as the Python twin unchanged: the schema entries cover the whole
# body (dropping what is unread), so accept/reject matches the twin exactly, and
# every leaf is a plain int on both sides — no casting twin (contrast the `bool`
# rule the dedust_v2 withdraw port had to follow).
#
# HOST FN (the payout half): `tonco_withdraw_payouts` wraps the shared core
# resolve_tonco_withdraw_payouts (blocks/liquidity.py), which both this spec and
# the legacy matcher call. Three things keep it host-side, and only these three:
#   - ToncoRouterV3PayTo cannot be a schema message at all: its coinsinfo is a
#     Maybe ^Cell (there is no `maybe_ref { }` composite) and its two tail refs
#     are conditional on exit_code, which the schema grammar has no form for;
#   - a leg with NO transfer block falls back to the router's declared slot and
#     needs an async jetton-wallet interface lookup plus the pTON-master
#     classification to decide whether that slot is TON or a jetton. `lookup
#     jetton_wallet(...)` plus three address literals could express the lookup in
#     isolation (specs_decl/pton_transfer.mch does exactly that), but the key it
#     would need comes out of the unparseable PayTo body;
#   - the final leg->slot reordering compares a resolved wallet against the
#     router's asset0 wallet, so it depends on both of the above.
# The legacy `additional_blocks` bookkeeping (the pTON call under a payout leg)
# is NOT needed here: `maybe op PTonTransfer` already consumes it, so the
# synthesized merge covers it — same as the deposit matcher above.
#
# DEVIATION (one, documented not worked around): the legacy build wraps the
# PoolV3Burn parse in try/except and keeps the anchor's burned_index when it
# throws, whereas a declarative `parse` REJECTS the whole match on a parse
# failure. The schema entry parses the full contract-fixed body, so this can only
# diverge on a malformed 0xd73ac09d body, of which the corpus has none.
#
# ANCHOR / ORDERING. 0x530b5f2c, 0x46ca335a and 0xd73ac09d are claimed by no
# other spec, so this matcher steals nothing and nothing steals from it.
# 0xa1daa96d IS shared — the deposit matcher above binds it as its refund router
# call, and specs/tonco_swap.mch binds it under a 0xa7fb58f8 swap — but in all
# three it is a non-anchor child of a different parent, and one message has one
# parent, so the three uses are disjoint. Within this file the withdraw matcher
# is listed after the deposit one, mirroring event_processing.matchers (deposit
# 64, deploy-pool 65, withdraw 66). What IS load-bearing is the default priority
# 100: the payout legs are `btype jetton_transfer`, so the priority-90 jetton
# specs must have built them first.

matcher tonco_withdraw_liquidity {
    produces tonco_withdraw_liquidity
    entry    @start_burn

    op ToncoPoolV3StartBurn @start_burn
      -> op ToncoPositionNftV3PositionBurn @position_burn
        -> op ToncoPoolV3Burn @pool_burn
          -> op ToncoRouterV3PayTo
            -> { maybe btype jetton_transfer @payout_1
                   -> { maybe op PTonTransfer }
               ; maybe btype jetton_transfer @payout_2
                   -> { maybe op PTonTransfer } }

    parse start_burn    as ToncoPoolV3StartBurn
    parse position_burn as ToncoPositionNftV3PositionBurn
    parse pool_burn     as ToncoPoolV3Burn

    let r = tonco_withdraw_payouts(consumed)
    reject when r == null
    failed when r.failed
    out {
        sender:              account(position_burn.body.nft_owner)
        pool:                account(start_burn.msg.destination)
        burned_nft_index:    pool_burn.body.burned_index if pool_burn.body.burned_index != 0 else start_burn.body.burned_index
        burned_nft_address:  account(position_burn.msg.destination)
        liquidity_burnt:     amount(start_burn.body.liquidity_to_burn)
        tick_lower:          start_burn.body.tick_lower
        tick_upper:          start_burn.body.tick_upper
        amount1_out:         r.amount1_out
        asset1_out:          r.asset1_out
        dex_wallet_1:        r.dex_wallet_1
        dex_jetton_wallet_1: r.dex_jetton_wallet_1
        wallet1:             r.wallet1
        amount2_out:         r.amount2_out
        asset2_out:          r.asset2_out
        dex_wallet_2:        r.dex_wallet_2
        dex_jetton_wallet_2: r.dex_jetton_wallet_2
        wallet2:             r.wallet2
    }
}
