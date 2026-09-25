# Matches DeDust V2 liquidity mining reward claims from the claim request through the reward payout.

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
