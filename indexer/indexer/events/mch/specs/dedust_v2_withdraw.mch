# DeDust V2 liquidity withdrawal covers position share burning, two optional payout legs, and slippage refunds.

import "lib/leg.mch"
import "lib/opcodes.mch"

opcode DedustV2Withdraw          = 0x20B5EF89
opcode DedustV2WithdrawLiquidity = 0xBC1531A9
opcode DedustV2ExitLiquidity     = 0xF6F6A3AA
opcode DedustV2WithdrawalEvent   = 0xC0D77B54

matcher dedust_v2_withdraw {
    produces        dex_withdraw_liquidity
    include_excess  false

    op DedustV2Withdraw @withdraw as DedustV2Withdraw
      -> op DedustV2WithdrawLiquidity
        -> op DedustV2ExitLiquidity
          -> { maybe op DedustV2WithdrawalEvent @event as DedustV2WithdrawalEvent
             ; maybe or {
                         op PayoutMessage @ton_1 as PayoutMessage
                         btype jetton_transfer @jt_1
                     }
             ; maybe or {
                         op PayoutMessage @ton_2 as PayoutMessage
                         btype jetton_transfer @jt_2
                     } }

    # Withdrawals with autoClaimFees mix fee payouts with withdrawn amounts and are rejected here.
    reject when withdraw.body.auto_claim_fees

    let success = event != null

    let a_1   = jt_1.data.amount ?? ton_1.body.amount
    let a_2   = jt_2.data.amount ?? ton_2.body.amount

    failed when not success

    let l1 = leg(jt_1 ?? ton_1)
    let l2 = leg(jt_2 ?? ton_2)

    out {
        dex:                 "dedust_v2"
        sender:              withdraw.msg.source
        sender_wallet:       null
        pool:                withdraw.msg.destination
        asset:               null
        lp_tokens_burnt:     amount(event.body.liquidity) if success else amount(withdraw.body.liquidity)
        is_refund:           not success
        amount1_out:         amount(a_1)
        asset1_out:          l1.asset
        dex_wallet_1:        l1.source
        dex_jetton_wallet_1: l1.source_wallet
        wallet1:             l1.destination_wallet
        amount2_out:         amount(a_2)
        asset2_out:          l2.asset
        dex_wallet_2:        l2.source
        dex_jetton_wallet_2: l2.source_wallet
        wallet2:             l2.destination_wallet
    }
}
