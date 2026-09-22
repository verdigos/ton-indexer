# Matches partial and completed DeDust liquidity deposits from native or jetton vault legs.

import "lib/opcodes.mch"

opcode DedustDepositTONToVault               = 0xd55e4686
opcode DedustAskLiquidityFactory             = 0xf04ec526
opcode DedustDeployDepositContract           = 0x9b3aa3fa
opcode DedustTopUpLiquidityDepositContract   = 0x54240fe5
opcode DedustDepositLiquidityToPool          = 0xb56b9598
opcode DedustDestroyLiquidityDepositContract = 0xaae79256
opcode DedustReturnExcessFromVault           = 0x6b0b787f
opcode DedustRejection                       = 0xe1a36cd4

rule deposit_leg = or {
                       op DedustDepositTONToVault
                       btype jetton_transfer
                       op JettonNotify <- op JettonInternalTransfer <- op JettonTransfer
                   }

rule vault_excess =
    op DedustReturnExcessFromVault
      -> { maybe btype jetton_transfer
         ; maybe op DedustPayout }

matcher dedust_deposit_final {
    produces dedust_deposit_liquidity

    op DedustDepositLiquidityToPool @pool {
        -> {
            or {
                op JettonInternalTransfer -> maybe op JettonNotify
                op DedustRejection
                  -> { maybe btype ton_transfer ; many[0..] $vault_excess }
            }
            ; maybe op DedustDestroyLiquidityDepositContract
                -> { maybe btype ton_transfer ; many[0..] $vault_excess }
        }
        <- op DedustTopUpLiquidityDepositContract <- op DedustAskLiquidityFactory {
            -> { maybe op DedustDeployDepositContract }
            <- $deposit_leg
        }
    }

    require r = dedust_deposit_final_data(consumed)
    require pool_state = lookup dedust_pool(r.pool_address)
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
