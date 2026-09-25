# Matches DeDust liquidity withdrawals with two pool payout legs.

import "lib/leg.mch"
import "lib/opcodes.mch"


matcher dedust_withdraw {
    produces dex_withdraw_liquidity

    op JettonBurn @burn as JettonBurn
      -> op JettonBurnNotification @notification
      -> { op DedustPayoutFromPool @req_1 as DedustPayoutFromPool -> or {
                                                                         op DedustPayout @ton_1
                                                                         btype jetton_transfer @jt_1
                                                                     }
         ; op DedustPayoutFromPool @req_2 as DedustPayoutFromPool -> or {
                                                                         op DedustPayout @ton_2
                                                                         btype jetton_transfer @jt_2
                                                                     } }

    require lp_wallet = lookup jetton_wallet(burn.msg.destination)
    let l1 = leg(jt_1 ?? ton_1)
    let l2 = leg(jt_2 ?? ton_2)

    out {
        dex:                 "dedust"
        sender:              burn.msg.source
        sender_wallet:       burn.msg.destination
        pool:                notification.msg.destination
        asset:               asset(lp_wallet.jetton)
        lp_tokens_burnt:     amount(burn.body.amount)
        is_refund:           false
        amount1_out:         amount(req_1.body.amount)
        asset1_out:          l1.asset
        dex_wallet_1:        l1.source
        dex_jetton_wallet_1: l1.source_wallet
        wallet1:             l1.destination_wallet
        amount2_out:         amount(req_2.body.amount)
        asset2_out:          l2.asset
        dex_wallet_2:        l2.source
        dex_jetton_wallet_2: l2.source_wallet
        wallet2:             l2.destination_wallet
    }
}
