# Matches Stonfi V2 liquidity withdrawals and refund payouts.

import "lib/opcodes.mch"
opcode StonfiV2WithdrawFromPool      = 0x297437cf
opcode StonfiV2WithdrawRefund        = 0x0f98e2b8
opcode StonfiV2WithdrawRefundParent  = 0x132b9a2c

matcher stonfi_v2_withdraw {
    produces dex_withdraw_liquidity

    btype jetton_burn @burn
      -> op StonfiV2WithdrawFromPool
           -> { op StonfiV2PayTo @payout_1 -> btype jetton_transfer @jt_1 -> maybe op PTonTransfer @pton_1 as PTonTransfer
              ; op StonfiV2PayTo -> btype jetton_transfer @jt_2 -> maybe op PTonTransfer @pton_2 as PTonTransfer }

    out {
        dex:                 "stonfi_v2"
        sender:              burn.data.owner
        sender_wallet:       burn.data.jetton_wallet
        pool:                payout_1.msg.source
        asset:               burn.data.asset
        lp_tokens_burnt:     burn.data.amount
        is_refund:           false
        amount1_out:         amount(pton_1.body.ton_amount ?? 0) if pton_1 != null else jt_1.data.amount
        asset1_out:          ton_asset() if pton_1 != null else jt_1.data.asset
        dex_wallet_1:        jt_1.data.sender
        dex_jetton_wallet_1: jt_1.data.sender_wallet
        wallet1:             jt_1.data.receiver_wallet
        amount2_out:         amount(pton_2.body.ton_amount ?? 0) if pton_2 != null else jt_2.data.amount
        asset2_out:          ton_asset() if pton_2 != null else jt_2.data.asset
        dex_wallet_2:        jt_2.data.sender
        dex_jetton_wallet_2: jt_2.data.sender_wallet
        wallet2:             jt_2.data.receiver_wallet
    }
}

matcher stonfi_v2_withdraw_refund {
    produces dex_withdraw_liquidity

    op StonfiV2WithdrawRefund @refund {
        -> {
            op StonfiV2PayTo @payout_1 -> btype jetton_transfer @jt_1 -> maybe op PTonTransfer @pton_1 as PTonTransfer
            ; op StonfiV2PayTo -> btype jetton_transfer @jt_2 -> maybe op PTonTransfer @pton_2 as PTonTransfer
        }
        <- op StonfiV2WithdrawRefundParent @refund_parent
    }

    out {
        dex:                 "stonfi_v2"
        sender:              refund_parent.msg.source
        sender_wallet:       null
        pool:                payout_1.msg.source
        asset:               null
        lp_tokens_burnt:     null
        is_refund:           true
        amount1_out:         amount(pton_1.body.ton_amount ?? 0) if pton_1 != null else jt_1.data.amount
        asset1_out:          ton_asset() if pton_1 != null else jt_1.data.asset
        dex_wallet_1:        jt_1.data.sender
        dex_jetton_wallet_1: jt_1.data.sender_wallet
        wallet1:             jt_1.data.receiver_wallet
        amount2_out:         amount(pton_2.body.ton_amount ?? 0) if pton_2 != null else jt_2.data.amount
        asset2_out:          ton_asset() if pton_2 != null else jt_2.data.asset
        dex_wallet_2:        jt_2.data.sender
        dex_jetton_wallet_2: jt_2.data.sender_wallet
        wallet2:             jt_2.data.receiver_wallet
    }
}
