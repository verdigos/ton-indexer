# Dedust two-branch liquidity deposit — ported from blocks/liquidity.py
# DedustDepositBlockMatcher (final) + DedustDepositFirstAssetBlockMatcher
# (partial). Registration order inside this file mirrors event_processing:
# final first, partial second.
#
# Key deviation from the hand-written pair, forced by registration order: the
# legacy matchers run BEFORE JettonTransferBlockMatcher, so they match the
# user's deposit leg as a RAW JettonTransfer -> JettonInternalTransfer ->
# JettonNotify chain and build the vault's jetton-excess payouts themselves
# (labeled(..., JettonTransferBlockMatcher, call_build_block=True)). Here
# jetton_transfer has already run, so the deposit leg and the excess payouts
# arrive as prebuilt `btype jetton_transfer` blocks; the raw notify chain is
# kept as a fallback arm for legs the jetton matcher rejected. Consumed
# event-node sets are identical either way (the composite carries the same
# nodes, including per-node excess absorption).
#
# The implementation's opcode values and rejection/destroy shapes are
# authoritative; this file follows the code rather than the idealized example.
#
# Paired partial+final blocks are merged by host-side
# post_process_dedust_liquidity: both builders return the exact
# registered block classes the pass dispatches on
# (DedustDepositLiquidity / DedustDepositLiquidityPartial).
#
# The partial matcher's topup guard (reject when a pool-deposit or rejection
# child follows the topup) stays in the host fn. Body fields come from ABI
# rows on the already-consumed deposit leg.
#
# The Ask relation block carries the deploy child and deposit-leg parent.
#
# Builders are consumed-set/tree-driven (Match._mch_consumed + previous_block
# navigation), same approach as dedust_swap/stonfi_v2; captures are kept for
# pattern fidelity only.

import "lib/opcodes.mch"

opcode DedustDepositTONToVault               = 0xd55e4686
opcode DedustAskLiquidityFactory             = 0xf04ec526
opcode DedustDeployDepositContract           = 0x9b3aa3fa
opcode DedustTopUpLiquidityDepositContract   = 0x54240fe5
opcode DedustDepositLiquidityToPool          = 0xb56b9598
opcode DedustDestroyLiquidityDepositContract = 0xaae79256
opcode DedustReturnExcessFromVault           = 0x6b0b787f
opcode DedustRejection                       = 0xe1a36cd4

# The deposit leg feeding the vault: TON straight to the vault, a prebuilt
# jetton transfer, or the raw notify chain (fallback, see header).
rule deposit_leg = or {
                       op DedustDepositTONToVault
                       btype jetton_transfer
                       op JettonNotify <- op JettonInternalTransfer <- op JettonTransfer
                   }

# One excess-liquidity refund from a vault: optional jetton payout (prebuilt
# transfer) and/or optional TON payout. Legacy: ContractMatcher(optional,
# ReturnExcessFromVault, children=[jetton_excess(optional), ton_excess(optional)]).
rule vault_excess =
    op DedustReturnExcessFromVault
      -> { maybe btype jetton_transfer
         ; maybe op DedustPayout }

# HYBRID declarative build. reject/failed/`dex` wiring and block assembly
# are declarative. Final's residue (deposit-leg resolution via previous_block,
# per-vault BOC parsing, jetton-wallet lookups, excess-payout scan) stays in
# dedust_deposit_final_data. Partial bodies parse as ABI rows in this matcher;
# dedust_deposit_partial_data is only the topup-child guard. Each fn returns
# null to reject. Product is a generic
# Block(btype dedust_deposit_liquidity[_partial]); the distinct btypes let
# post_process_dedust_liquidity dual-dispatch (isinstance legacy classes OR
# btype) so both paths pair identically.

matcher dedust_deposit_final {
    produces dedust_deposit_liquidity

    op DedustDepositLiquidityToPool @pool {
        -> {
            or {
                op JettonInternalTransfer -> maybe op JettonNotify
                op DedustRejection
                  -> { maybe btype ton_transfer ; maybe many $vault_excess }
            }
            ; maybe op DedustDestroyLiquidityDepositContract
                -> { maybe btype ton_transfer ; maybe many $vault_excess }
        }
        <- op DedustTopUpLiquidityDepositContract <- op DedustAskLiquidityFactory {
            -> { maybe op DedustDeployDepositContract }
            <- $deposit_leg
        }
    }

    require r = dedust_deposit_final_data(consumed)
    failed when not r.success
    out {
        dex:                  "dedust"
        sender:               r.sender
        pool_address:         r.pool_address
        deposit_contract:     r.deposit_contract
        lp_tokens_minted:     r.lp_tokens_minted
        asset_1:              r.asset_1
        asset_2:              r.asset_2
        amount_1:             r.amount_1
        amount_2:             r.amount_2
        target_asset_1:       r.target_asset_1
        target_amount_1:      r.target_amount_1
        target_asset_2:       r.target_asset_2
        target_amount_2:      r.target_amount_2
        user_jetton_wallet_1: r.user_jetton_wallet_1
        user_jetton_wallet_2: r.user_jetton_wallet_2
        vault_excesses:       r.vault_excesses
        success:              r.success
    }
}

matcher dedust_deposit_partial {
    produces dedust_deposit_liquidity_partial

    op DedustTopUpLiquidityDepositContract @topup
        <- op DedustAskLiquidityFactory {
            -> { op DedustDeployDepositContract }
            <- or {
                   op DedustDepositTONToVault @vault as DedustDepositLiquidityNative
                   btype jetton_transfer @jt
                   op JettonNotify @notify as JettonNotify <- op JettonInternalTransfer @internal <- op JettonTransfer
               }
        }

    let is_ton = vault != null
    let payload_cell = jt.data.forward_payload if jt != null else (tail_unwrap(notify.body.forward_payload_cell) if notify != null else null)
    let payload = parse? payload_cell as DedustDepositLiquidityJetton
    reject when not is_ton and payload == null
    let jw = lookup jetton_wallet(notify.msg.source) if notify != null else null
    reject when notify != null and jw == null
    require r = dedust_deposit_partial_data(consumed)
    out {
        dex:                  "dedust"
        sender:               (vault.msg.source ?? addr_none()) if is_ton else (jt.data.sender if jt != null else (account(notify.body.from_user) ?? addr_none()))
        deposit_contract:     topup.msg.destination
        asset_1:              ton_asset() if is_ton else (jt.data.asset if jt != null else asset(jw.jetton))
        amount_1:             amount(vault.body.amount) if is_ton else (jt.data.amount if jt != null else amount(notify.body.jetton_amount))
        asset_2:              null
        amount_2:             null
        user_jetton_wallet_1: addr_none() if is_ton else (jt.data.sender_wallet if jt != null else (account(internal.msg.source) ?? addr_none()))
        user_jetton_wallet_2: addr_none()
        target_asset_1:       asset_of(vault.body.asset0) if is_ton else asset_of(payload.asset0)
        target_amount_1:      amount(vault.body.params.ref.amount0) if is_ton else amount(payload.amount0)
        target_asset_2:       asset_of(vault.body.asset1) if is_ton else asset_of(payload.asset1)
        target_amount_2:      amount(vault.body.params.ref.amount1) if is_ton else amount(payload.amount1)
    }
}
