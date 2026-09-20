# Tonstakers matching covers deposits, immediate and delayed withdrawal requests, and delayed withdrawal payouts.

import "lib/opcodes.mch"
opcode TONStakersDepositRequest      = 0x47d54391
opcode TONStakersWithdrawRequest     = 0x319b0cdc
opcode TONStakersPoolWithdrawal      = 0x0a77535c
opcode TONStakersMintNFT             = 0x1674b0a0
opcode TONStakersInitNFT             = 0x132f9a45
# The pool reuses 0x1674b0a0 for jetton minting and request NFT minting, and the parent request disambiguates them.
opcode TONStakersMintJettons         = 0x1674b0a0
opcode TONStakersNftBurn             = 0xf127fe4e
opcode TONStakersNftBurnNotification = 0xed58b0b2
opcode TONStakersDistributedAsset    = 0xdb3b8abd

matcher tonstakers_delayed_withdrawal {
    produces tonstakers_withdraw

    op TONStakersDistributedAsset @distributed
      <- op TONStakersNftBurnNotification @notification as TONStakersNftBurnNotification
      <- op TONStakersNftBurn

    out {
        stake_holder: account(notification.body.owner)
        burnt_nft:    notification.msg.source
        pool:         tonstakers_pool_addr(notification)
        tokens_burnt: amount(notification.body.amount)
        amount:       amount(distributed.msg.value)
        asset:        null
    }
}

matcher tonstakers_deposit {
    produces tonstakers_deposit

    op TONStakersDepositRequest @deposit
      -> op TONStakersMintJettons
        -> op JettonInternalTransfer @transfer as JettonInternalTransfer
          -> maybe op JettonNotify

    let ok = not deposit.failed
    failed when deposit.failed
    out {
        source:             deposit.msg.source
        user_jetton_wallet: transfer.msg.destination if ok else null
        pool:               deposit.msg.destination
        value:              amount(deposit.msg.value - 1000000000)
        tokens_minted:      amount(transfer.body.amount) if ok else null
        asset:              asset(transfer.msg.source)
    }
}

matcher tonstakers_withdraw {
    produces tonstakers_withdraw | tonstakers_withdraw_request

    op JettonBurn @burn as JettonBurn
      -> op JettonBurnNotification
        -> op TONStakersWithdrawRequest @request
          -> xor {
                 op TONStakersPoolWithdrawal @immediate_withdrawal
                 op TONStakersMintNFT
                   -> { maybe peek btype nft_mint @minted_block
                      ; maybe peek op TONStakersInitNFT @minted_call }
             }

    let minted = (minted_block.msg.destination ?? addr_none()) if minted_block != null else ((minted_call.msg.destination ?? addr_none()) if minted_call != null else null)
    failed when burn.failed
    failed when immediate_withdrawal == null and minted == null
    produces switch {
        when immediate_withdrawal != null => tonstakers_withdraw out {
            stake_holder: burn.msg.source
            burnt_nft:    null
            pool:         request.msg.destination
            tokens_burnt: amount(burn.body.amount)
            amount:       amount(immediate_withdrawal.msg.value - request.msg.value)
            asset:        asset(request.msg.source)
        }
        else => tonstakers_withdraw_request out {
            source:       burn.msg.source
            tsTON_wallet: burn.msg.destination
            pool:         request.msg.destination
            tokens_burnt: amount(burn.body.amount)
            minted_nft:   minted
            asset:        asset(request.msg.source)
        }
    }
}
