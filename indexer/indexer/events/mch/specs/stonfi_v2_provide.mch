# Matches each Stonfi v2 liquidity deposit leg from the incoming transfer through the pool callback.

import "lib/leg.mch"
import "lib/opcodes.mch"

opcode StonfiV2ProvideLiquidity = 0x37c096df
opcode StonfiV2AddLiquidity     = 0x50c6a654
opcode StonfiV2CbAddLiquidity   = 0x06ecd527

rule in_leg = or {
                  op JettonNotify @notify_pton <- op PTonTransfer @pton_in
                  btype jetton_transfer @in_jt
                  op JettonNotify <- op JettonInternalTransfer <- btype jetton_transfer @in_jt_raw
              }

matcher stonfi_v2_provide_final {
    produces dex_deposit_liquidity

    op StonfiV2ProvideLiquidity @provide as StonfiV2ProvideLiquidity {
        -> {
            op StonfiV2AddLiquidity
                -> op StonfiV2CbAddLiquidity
                    -> or {
                           op JettonInternalTransfer @lp_transfer as JettonInternalTransfer
                           op StonfiV2AddLiquidity @refund
                       }
        }
        <- $in_leg
    }

    let l = leg(in_jt ?? in_jt_raw ?? notify_pton)
    failed when refund != null
    out {
        dex:             "stonfi_v2"
        amount_1:        amount(provide.body.amount1) if provide.body.amount1 > 0 else amount(provide.body.amount2)
        asset_1:         l.asset
        sender:          (in_jt ?? in_jt_raw).data.sender if (in_jt ?? in_jt_raw) != null else account(pton_in.msg.source)
        sender_wallet_1: l.source_wallet
        amount_2:        null
        asset_2:         null
        sender_wallet_2: null
        pool:            provide.msg.destination
        lp_tokens_minted: amount(lp_transfer.body.amount)
    }
}

matcher stonfi_v2_provide_partial {
    produces dex_deposit_liquidity

    op StonfiV2ProvideLiquidity @provide as StonfiV2ProvideLiquidity {
        -> { op StonfiV2AddLiquidity }
        <- $in_leg
    }

    let l = leg(in_jt ?? in_jt_raw ?? notify_pton)
    out {
        dex:             "stonfi_v2"
        amount_1:        amount(provide.body.amount1) if provide.body.amount1 > 0 else amount(provide.body.amount2)
        asset_1:         l.asset
        sender:          (in_jt ?? in_jt_raw).data.sender if (in_jt ?? in_jt_raw) != null else account(pton_in.msg.source)
        sender_wallet_1: l.source_wallet
        amount_2:        null
        asset_2:         null
        sender_wallet_2: null
        pool:            provide.msg.destination
        lp_tokens_minted: null
    }
}
