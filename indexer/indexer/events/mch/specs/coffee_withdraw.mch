# Matches Coffee liquidity withdrawals with two pool payouts and a withdrawal log.
# Payout order follows ascending min_lt, which determines which event amount labels each leg.

import "lib/opcodes.mch"
opcode CoffeeLiquidityWithdrawalEvent = 0xc0ffee35

matcher coffee_withdraw_liquidity {
    produces dex_withdraw_liquidity

    btype jetton_burn @burn
      -> { op CoffeePayoutInternal @payout_1
             -> xor {
                    btype jetton_transfer @jt_1
                    op CoffeePayout @ton_1
                    op CoffeeNotification
                }
         ; op CoffeePayoutInternal @payout_2
             -> xor {
                    btype jetton_transfer @jt_2
                    op CoffeePayout @ton_2
                    op CoffeeNotification
                }
         ; op CoffeeLiquidityWithdrawalEvent @event as CoffeeLiquidityWithdrawalEvent }

    out {
        dex:                 "coffee"
        sender:              burn.data.owner
        sender_wallet:       burn.data.jetton_wallet
        pool:                event.msg.source
        asset:               burn.data.asset
        lp_tokens_burnt:     burn.data.amount
        is_refund:           false
        amount1_out:         amount(event.body.amount1)
        asset1_out:          jt_1.data.asset if jt_1 != null else ton_asset()
        dex_wallet_1:        payout_1.msg.source
        dex_jetton_wallet_1: jt_1.data.sender_wallet
        wallet1:             jt_1.data.receiver_wallet if jt_1 != null else ton_1.msg.destination
        amount2_out:         amount(event.body.amount2)
        asset2_out:          jt_2.data.asset if jt_2 != null else ton_asset()
        dex_wallet_2:        payout_2.msg.source
        dex_jetton_wallet_2: jt_2.data.sender_wallet
        wallet2:             jt_2.data.receiver_wallet if jt_2 != null else ton_2.msg.destination
    }
}
