# Coffee staking matchers cover deposits, position withdrawals, and reward claims.

opcode CoffeeStakingDeposit             = 0xF9471134
opcode CoffeeStakingPositionWithdraw    = 0xCB03BFAF
opcode CoffeeStakingClaimRewards        = 0xB30C7310

matcher coffee_staking_deposit {
    produces coffee_staking_deposit

    btype jetton_transfer @transfer
      -> op CoffeeStakingDeposit @pool_call
        -> { maybe op CoffeeStakingDeposit
           ; btype nft_mint @nft }

    out {
        source:              transfer.data.sender
        user_jetton_wallet:  transfer.data.sender_wallet
        pool_jetton_wallet:  transfer.data.receiver_wallet
        pool:                pool_call.msg.destination
        value:               amount(transfer.data.amount)
        minted_item_address: nft.data.address
        minted_item_index:   nft.data.index
        asset:               transfer.data.asset
    }
}

matcher coffee_staking_withdraw {
    produces coffee_staking_withdraw

    op CoffeeStakingPositionWithdraw @withdraw_1
      -> op CoffeeStakingPositionWithdraw @withdraw_2 as CoffeeStakingPositionWithdraw2
        -> { op CoffeeStakingPositionWithdraw @withdraw_3 as CoffeeStakingPositionWithdraw3
               -> btype jetton_transfer @transfer
           ; maybe op CoffeeStakingPositionWithdraw }

    out {
        source:             account(withdraw_2.body.owner)
        pool:               withdraw_2.msg.source
        asset:              transfer.data.asset
        amount:             amount(withdraw_3.body.jetton_amount ?? 0)
        user_jetton_wallet: transfer.data.receiver_wallet
        pool_jetton_wallet: account(withdraw_3.body.jetton_wallet)
        nft_address:        withdraw_1.msg.destination
        nft_index:          withdraw_2.body.nft_id
        points:             withdraw_2.body.points ?? 0
    }
}

matcher coffee_staking_claim_rewards {
    produces coffee_staking_claim_rewards

    op CoffeeStakingClaimRewards @claim as CoffeeStakingClaimRewards
      -> { btype jetton_transfer @transfer
         ; maybe op CoffeeStakingClaimRewards }

    out {
        admin:                   claim.msg.source
        recipient:               account(claim.body.receiver)
        pool:                    claim.msg.destination
        asset:                   transfer.data.asset
        amount:                  amount(claim.body.jetton_amount ?? 0)
        pool_jetton_wallet:      account(claim.body.jetton_wallet)
        recipient_jetton_wallet: transfer.data.receiver_wallet
    }
}
