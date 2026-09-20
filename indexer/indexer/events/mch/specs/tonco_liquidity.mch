# Tonco liquidity covers deposits, pool deployment, and withdrawals; withdrawal exit code 201 counts as a successful payout.

import "lib/opcodes.mch"
opcode ToncoAccountV3AddLiquidity      = 0x3ebe5431
opcode ToncoPoolV3FundAccount          = 0x4468de77
opcode ToncoPoolV3MinAndRefund         = 0x81702ef8
opcode ToncoPoolV3StartBurn            = 0x530b5f2c
opcode ToncoPositionNftV3PositionBurn  = 0x46ca335a
opcode ToncoPoolV3Burn                 = 0xd73ac09d
opcode ToncoRouterV3CreatePool         = 0x2e3034ef
opcode ToncoPoolV3Init                 = 0x441c39ed
matcher tonco_deposit_liquidity {
    produces tonco_deposit_liquidity

    op ToncoAccountV3AddLiquidity @add_liquidity as ToncoAccountV3AddLiquidity {
        -> {
            maybe op ToncoPoolV3MinAndRefund
                -> { maybe op ToncoRouterV3PayTo
                        -> { maybe btype jetton_transfer @excess_1
                                -> { maybe op PTonTransfer }
                           ; maybe btype jetton_transfer @excess_2
                                -> { maybe op PTonTransfer } }
                   ; btype nft_mint @nft_mint
                   ; maybe op Excess }
        }
        <- op ToncoPoolV3FundAccount <- or {
                                            op JettonNotify <- op PTonTransfer
                                            btype jetton_transfer
                                        }
    }

    reject when add_liquidity.body == null
    require r = tonco_deposit_liquidity_data(add_liquidity, excess_1, excess_2, nft_mint)
    let is_first = add_liquidity.body.new_amount0 > 0
    out {
        sender:            r.sender
        pool:              add_liquidity.msg.source
        account_contract:  add_liquidity.msg.destination
        position_amount_1: amount(add_liquidity.body.new_enough0)
        position_amount_2: amount(add_liquidity.body.new_enough1)
        lp_tokens_minted:  r.lp_tokens_minted
        tick_lower:        add_liquidity.body.tick_lower
        tick_upper:        add_liquidity.body.tick_upper
        nft_index:         r.nft_index
        nft_address:       nft_mint.data.address if nft_mint != null else null
        amount_1:          r.sent_amount if is_first else null
        asset_1:           r.first_asset if is_first else r.second_asset
        sender_wallet_1:   r.sender_wallet if is_first else null
        amount_2:          null if is_first else r.sent_amount
        asset_2:           r.second_asset if is_first else r.first_asset
        sender_wallet_2:   null if is_first else r.sender_wallet
        excesses:          r.excesses
    }
}


matcher tonco_deploy_pool {
    produces tonco_deploy_pool

    op ToncoRouterV3CreatePool @create as ToncoRouterV3CreatePool
      -> op ToncoPoolV3Init @init as ToncoPoolV3Init
        -> maybe op Excess

    out {
        deployer:              create.msg.source
        router:                create.msg.destination
        pool:                  init.msg.destination
        jetton0_router_wallet: account(create.body.jetton_wallet0)
        jetton1_router_wallet: account(create.body.jetton_wallet1)
        jetton0_minter:        account(create.body.minters.ref.jetton0_minter)
        jetton1_minter:        account(create.body.minters.ref.jetton1_minter)
        tick_spacing:          create.body.tick_spacing
        initial_price_x96:     create.body.initial_price_x96
        protocol_fee:          create.body.protocol_fee
        lp_fee_base:           create.body.lp_fee_base
        lp_fee_current:        create.body.lp_fee_current
        pool_active:           init.body.pool_active
        success:               true
    }
}



matcher tonco_withdraw_liquidity {
    produces tonco_withdraw_liquidity

    op ToncoPoolV3StartBurn @start_burn as ToncoPoolV3StartBurn
      -> op ToncoPositionNftV3PositionBurn @position_burn as ToncoPositionNftV3PositionBurn
        -> op ToncoPoolV3Burn @pool_burn as ToncoPoolV3Burn
          -> op ToncoRouterV3PayTo @pay_to as ToncoRouterV3PayTo
            -> { maybe btype jetton_transfer @payout_1
                   -> { maybe op PTonTransfer }
               ; maybe btype jetton_transfer @payout_2
                   -> { maybe op PTonTransfer } }


    require r = tonco_withdraw_payouts(pay_to, pay_to.body, payout_1, payout_2)
    failed when pay_to.body.exit_code != 0 and pay_to.body.exit_code != 201
    out {
        sender:              account(position_burn.body.nft_owner)
        pool:                start_burn.msg.destination
        burned_nft_index:    pool_burn.body.burned_index if pool_burn.body.burned_index != 0 else start_burn.body.burned_index
        burned_nft_address:  position_burn.msg.destination
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
