# Coffee matchers cover liquidity deposits and vault creation.

import "lib/leg.mch"
import "lib/opcodes.mch"

opcode CoffeeDepositLiquidityNative           = 0xc0ffee04
opcode CoffeeCreateVault                      = 0xc0ffee06
opcode CoffeeCreateLiquidityDepositoryRequest = 0xc0ffee27
opcode CoffeeDepositLiquidityInternal         = 0xc0ffee29
opcode ProvideWalletAddress                   = 0x2c76b973
opcode TakeWalletAddress                      = 0xd1735400

matcher coffee_deposit_liquidity {
    produces coffee_deposit_liquidity

    op CoffeeCreateLiquidityDepositoryRequest @request as CoffeeCreateLiquidityDepositoryRequest {
        -> {
            op CoffeeDeploy @deploy
                -> maybe op CoffeeDepositLiquidityInternal
                    -> { op JettonInternalTransfer -> maybe op Excess
                       ; op CoffeeDepositLiquiditySuccessfulEvent @success as CoffeeDepositLiquiditySuccessfulEvent
                       ; maybe op CoffeePayoutInternal
                            -> xor {
                                   btype jetton_transfer @out_jt
                                   op CoffeePayout @out_call
                                   op CoffeeNotification @out_call
                               } }
        }
        <- xor {
               op CoffeeDepositLiquidityNative @in_native
               btype jetton_transfer @in_jt
           }
    }


    let l = leg(in_jt ?? in_native)
    let pool_first = asset_of(request.body.pool_params.ref.pool_first)
    let pool_second = asset_of(request.body.pool_params.ref.pool_second)
    let is_first   = l.asset == pool_first
    reject when not (is_first or l.asset == pool_second)

    let in_amount = amount(request.body.amount)
    let excess_jetton = [[out_jt.data.asset, out_jt.data.amount]] if out_jt != null else null
    let excess_ton    = [[ton_asset(), amount(out_call.msg.value ?? 0)]] if out_call != null else null
    out {
        dex:                  "coffee"
        sender:               account(request.body.sender) ?? addr_none()
        pool_address:         success.msg.source
        deposit_contract:     deploy.msg.destination
        vault_excesses:       excess_jetton ?? excess_ton ?? []
        asset_1:              l.asset if is_first else null
        amount_1:             in_amount if is_first else null
        user_jetton_wallet_1: l.source_wallet if is_first else null
        asset_2:              null if is_first else l.asset
        amount_2:             null if is_first else in_amount
        user_jetton_wallet_2: null if is_first else l.source_wallet
        target_asset_1:       pool_first if is_first else pool_second
        target_amount_1:      null
        target_asset_2:       pool_second if is_first else pool_first
        target_amount_2:      null
        lp_tokens_minted:     amount(success.body.lp_amount)
    }
}

matcher coffee_create_vault {
    produces coffee_create_vault

    op CoffeeCreateVault @create as CoffeeCreateVault
      -> op CoffeeDeploy @deploy
      -> maybe op ProvideWalletAddress
      -> maybe op TakeWalletAddress

    out {
        sender: create.msg.source
        vault:  deploy.msg.destination
        asset:  asset_of(create.body.asset)
        amount: amount(create.msg.value)
    }
}
