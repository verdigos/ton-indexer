# Matches DeDust V2 liquidity position fee claims with up to two payout legs.

import "lib/leg.mch"
import "lib/opcodes.mch"

opcode ClaimPositionFees            = 0x5652f1df
opcode ClaimAvailableFees           = 0x25f19752
opcode DedustV2PayoutPositionFees   = 0x29ff1bcf

matcher dedust_v2_claim_fees {
    produces dedust_v2_claim_fees

    op ClaimPositionFees @claim
      -> op ClaimAvailableFees
        -> op DedustV2PayoutPositionFees @payout_fees as DedustV2PayoutPositionFees
          -> { maybe or {
                         op PayoutMessage @ton_1 as PayoutMessage
                         btype jetton_transfer @jt_1
                     }
             ; maybe or {
                         op PayoutMessage @ton_2 as PayoutMessage
                         btype jetton_transfer @jt_2
                     } }

    let l1 = leg(jt_1 ?? ton_1)
    let a_1     = jt_1.data.amount.value ?? ton_1.body.amount

    let l2 = leg(jt_2 ?? ton_2)
    let a_2     = jt_2.data.amount.value ?? ton_2.body.amount

    let xf     = payout_fees.body.x_fees
    let yf     = payout_fees.body.y_fees
    let lx = l1 if a_1 == xf else (l2 if a_2 == xf else null)
    let ly = l1 if (a_1 != xf and a_1 == yf) else (l2 if (not (a_2 == xf and a_1 != xf) and a_2 == yf) else null)

    out {
        dex:                 "dedust_v2"
        sender:              claim.msg.source
        pool:                claim.msg.destination
        position:            payout_fees.msg.source
        owner:               account(payout_fees.body.owner)
        amount_x:            amount(xf)
        amount_y:            amount(yf)
        asset_x:             lx.asset
        asset_y:             ly.asset
        user_wallet_x:       lx.destination_wallet
        user_wallet_y:       ly.destination_wallet
        dex_wallet_x:        lx.source
        dex_wallet_y:        ly.source
        dex_jetton_wallet_x: lx.source_wallet
        dex_jetton_wallet_y: ly.source_wallet
    }
}
