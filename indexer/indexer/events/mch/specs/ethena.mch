# Ethena matchers cover deposits and withdrawal requests.

matcher ethena_deposit {
    produces ethena_deposit

    btype jetton_transfer @transfer -> btype jetton_mint @mint

    reject when transfer.data.asset != "0:086FA2A675F74347B08DD4606A549B8FDB98829CB282BC1949D3B12FBAED9DCC"
    reject when mint.data.asset     != "0:D0E545323C7ACB7102653C073377F7E3C67F122EB94D430A250739F109D4A57D"
    out {
        source:             account(transfer.data.sender)
        user_jetton_wallet: account(transfer.data.sender_wallet)
        pool:               account(transfer.data.receiver)
        value:              amount(transfer.data.amount)
        tokens_minted:      amount(mint.data.amount)
        asset:              mint.data.asset
        source_asset:       transfer.data.asset
    }
}

import "lib/opcodes.mch"
opcode EthenaTimelockTransfer  = 0xb2583ed5

matcher ethena_withdrawal_request {
    produces ethena_withdrawal_request

    op JettonTransfer @transfer
      -> op JettonInternalTransfer @internal_transfer as JettonInternalTransfer
        -> op JettonNotify
          -> op JettonMint @mint
            -> op EthenaTimelockTransfer @ts_usde_transfer as JettonInternalTransfer

    let src_wallet_info = lookup jetton_wallet(transfer.msg.destination)
    let dst_wallet_info = lookup jetton_wallet(ts_usde_transfer.msg.destination)
    # Jetton wallet lookup miss declines this request and leaves the trace for other matchers.
    reject when src_wallet_info == null
    reject when dst_wallet_info == null
    reject when account(src_wallet_info.owner)  != transfer.msg.source
    reject when account(dst_wallet_info.owner)  != transfer.msg.source
    reject when account(src_wallet_info.jetton) != internal_transfer.msg.destination
    reject when account(dst_wallet_info.jetton) != ts_usde_transfer.msg.source
    out {
        source:          transfer.msg.source
        source_wallet:   transfer.msg.destination
        receiver_wallet: ts_usde_transfer.msg.destination
        pool:            mint.msg.source
        asset:           account(src_wallet_info.jetton)
        amount:          amount(internal_transfer.body.amount)
        ts_usde_amount:  amount(ts_usde_transfer.body.amount)
    }
}
