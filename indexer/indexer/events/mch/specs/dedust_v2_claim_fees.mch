# DeDust CPMM V2 LP trading-fees claim. Sibling of specs/dedust_v2_claim_reward.mch
# (same shape, two payout slots instead of one), ported from the reference
# DedustV2ClaimFeesBlockMatcher (blocks/dedust_v2/claims.py:24).
#
# Chain:
#   ClaimPositionFees 0x5652f1df (user -> Pool)
#     ClaimAvailableFees 0x25f19752 (Pool -> Position)   # matched by opcode only
#       PayoutPositionFees 0x29ff1bcf (Position -> Pool) # exact x/y fees + owner
#         payout leg  (PayoutMessage 0x3216ca09 TON | jetton_transfer)  # optional
#         payout leg  (same)                                            # optional
#         excess 0xd53276db                              # auto-absorbed
#
# PARSER PATHS, mixed in one spec (legal; specs/dedust_v2_swap.mch does the
# same): PayoutPositionFees comes from the protocol ABI entry of the same name
# (snake_case, ABI prefix checked) with the LEGACY class as the Python twin,
# while PayoutMessage rides the existing ton-abi bridge row (camelCase). Both sit
# behind an `op` guard, which the ABI path requires.
#
# SLOT ASSIGNMENT, the one non-obvious part. Legacy does not read amounts off the
# legs positionally — it matches each leg to the x or y slot by EXACT amount
# against PayoutPositionFees, first-match-wins, and drops a leg that matches
# neither (claims.py:71-83):
#
#     for payout in sorted-by-min_lt:
#         if amount == x_fees and x empty: x = leg
#         elif amount == y_fees and y empty: y = leg
#
# Unrolled over the two legs the language can bind, that is exactly:
#     x = leg1 if a1 == x_fees else (leg2 if a2 == x_fees else null)
#     y = leg1 if (a1 != x_fees and a1 == y_fees)
#         else (leg2 if (not (a2 == x_fees and a1 != x_fees)) and a2 == y_fees
#               else null)
# — the `x_is_1 / x_is_2 / y_is_1 / y_is_2` lets below. A leg that never bound has
# a null amount, which compares unequal to both fees, so the zero-leg and one-leg
# shapes fall out for free (legacy's `legs["x"] or empty`).
#
# The action is `dedust_v2_claim_fees`, reusing the dex_withdraw_liquidity_data
# composite; `out` keys are the REFERENCE matcher's data keys so the SAME
# serializer fill (_fill_dedust_v2_claim_fees) serves both the legacy block and
# this one.

import "lib/leg.mch"

opcode ClaimPositionFees            = 0x5652f1df
opcode ClaimAvailableFees           = 0x25f19752
opcode DedustV2PayoutPositionFees   = 0x29ff1bcf
opcode PayoutMessage                = 0x3216ca09

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


    # Per-leg projection (parse_payout_leg, dedust_v2/common.py:28): a jetton leg
    # reads the prebuilt block's data, a TON leg is (amount, TON, no user wallet,
    # the payer, no dex jetton wallet).
    let l1 = leg(jt_1 ?? ton_1)
    let a_1     = jt_1.data.amount.value ?? ton_1.body.amount

    let l2 = leg(jt_2 ?? ton_2)
    let a_2     = jt_2.data.amount.value ?? ton_2.body.amount

    let xf     = payout_fees.body.x_fees
    let yf     = payout_fees.body.y_fees
    let x_is_1 = a_1 == xf
    let x_is_2 = not x_is_1 and a_2 == xf
    let y_is_1 = not x_is_1 and a_1 == yf
    let y_is_2 = not y_is_1 and not (a_2 == xf and not x_is_1) and a_2 == yf

    out {
        dex:                 "dedust_v2"
        sender:              claim.msg.source
        pool:                claim.msg.destination
        position:            payout_fees.msg.source
        owner:               account(payout_fees.body.owner)
        amount_x:            amount(xf)
        amount_y:            amount(yf)
        asset_x:             l1.asset if x_is_1 else (l2.asset if x_is_2 else null)
        asset_y:             l1.asset if y_is_1 else (l2.asset if y_is_2 else null)
        user_wallet_x:       l1.destination_wallet if x_is_1 else (l2.destination_wallet if x_is_2 else null)
        user_wallet_y:       l1.destination_wallet if y_is_1 else (l2.destination_wallet if y_is_2 else null)
        dex_wallet_x:        l1.source if x_is_1 else (l2.source if x_is_2 else null)
        dex_wallet_y:        l1.source if y_is_1 else (l2.source if y_is_2 else null)
        dex_jetton_wallet_x: l1.source_wallet if x_is_1 else (l2.source_wallet if x_is_2 else null)
        dex_jetton_wallet_y: l1.source_wallet if y_is_1 else (l2.source_wallet if y_is_2 else null)
    }
}
