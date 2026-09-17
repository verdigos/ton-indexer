# DeDust CPMM V2 liquidity-mining reward claim corresponding to
# DedustV2ClaimRewardBlockMatcher (blocks/dedust_v2/
# claims.py). The three message bodies are parsed through the ton-abi generated
# structs on the C++ side (bridge rows ClaimRewardMessage / PayoutRewardMessage /
# PayoutMessage) and hand parsers on the Python twin (builders/dedust_v2.py). The
# `parse ... as <Name>` names are the BARE ABI decl names, matching both.
#
# Chain shape:
#   ClaimReward 0x909fdb65 (user -> Pool)
#     ClaimPositionReward 0x2e0cccba (Pool -> Position)   # matched by opcode only
#       PayoutReward 0x9e17fbbe (Position -> Pool)        # exact amount + owner
#         payout leg  (PayoutMessage 0x3216ca09 TON | jetton_transfer)   # optional
#         excess 0xd53276db                               # auto-absorbed (default)
#
# Anchored on the payout-back message:
# the request alone doesn't carry the amount. Action mirrors the reference:
# type dedust_v2_claim_reward reusing the dex_withdraw_liquidity_data shape,
# amount1 = PayoutReward.amount, asset1/wallets from the payout leg, side-2 null.
#
# `out` keys are the REFERENCE matcher's data keys (blocks/dedust_v2/claims.py
# DedustV2ClaimRewardBlockMatcher), so the SAME serializer fill
# (_fill_dedust_v2_claim_reward) serves both the legacy block and this one.

import "lib/leg.mch"
import "lib/opcodes.mch"

opcode ClaimRewardMessage   = 0x909fdb65
opcode ClaimPositionReward  = 0x2e0cccba
opcode PayoutRewardMessage  = 0x9e17fbbe

matcher dedust_v2_claim_reward {
    produces dedust_v2_claim_reward

    op ClaimRewardMessage @claim as ClaimRewardMessage
      -> op ClaimPositionReward
      -> op PayoutRewardMessage @payout_reward as PayoutRewardMessage
      -> maybe or {
                   op PayoutMessage @ton as PayoutMessage
                   btype jetton_transfer @jt
               }

    let l = leg(jt ?? ton)

    out {
        dex:               "dedust_v2"
        sender:            claim.msg.source
        pool:              claim.msg.destination
        position:          payout_reward.msg.source
        owner:             payout_reward.body.ownerAddress
        reward_index:      claim.body.rewardIndex
        amount:            amount(payout_reward.body.amount)
        asset:             l.asset
        user_wallet:       l.destination_wallet
        dex_jetton_wallet: l.source_wallet
        dex_wallet:        l.source
    }
}
