# Stonfi V1 swaps cover incoming jettons, payment requests, and outgoing transfers.

opcode StonfiSwap           = 0x25938561
opcode StonfiPaymentRequest = 0xf93bb43f

matcher stonfi_v1_swap {
    produces jetton_swap

    btype jetton_transfer @in_transfer
      -> op StonfiSwap @swap as StonfiSwapMessage
      -> op StonfiPaymentRequest where ((parse? . as StonfiPaymentRequest).exit_code in [0xc64370e5, 0x5ffe1295, 0x38976e9b])
      -> btype jetton_transfer

    let d = stonfi_v1_swap_data(in_transfer, swap)
    reject when d.outgoing_transfer == null

    let payload            = in_transfer.data.stonfi_swap_body
    let out_wallet_address = payload.jetton_wallet if payload != null else d.out_address

    # Output wallet lookup miss leaves the destination asset unknown.
    # Outgoing wallet lookup miss treats the outgoing asset as TON.
    let out_wallet        = lookup jetton_wallet(out_wallet_address)
    let actual_out_wallet = lookup jetton_wallet(d.out_address)
    let dex_in_wallet     = lookup jetton_wallet(swap.body.token_wallet)

    reject when out_wallet_address == null or account(out_wallet_address) == addr_none()
    reject when d.out_address == null or account(d.out_address) == addr_none()
    reject when swap.body.token_wallet == null or account(swap.body.token_wallet) == addr_none()
    reject when dex_in_wallet == null

    let outgoing       = d.outgoing_transfer
    let outgoing_asset = asset(actual_out_wallet.jetton) ?? ton_asset()
    let incoming_asset = asset(dex_in_wallet.jetton) ?? ton_asset()
    let out_destination_wallet = outgoing.data.receiver_wallet if outgoing.data.has_internal_transfer else null
    let out_destination = outgoing.data.receiver if out_destination_wallet != null else (
        account(payload.user_address) if payload != null else account(swap.body.from_user_address))

    failed when not d.success
    out {
        dex:      "stonfi"
        sender:   in_transfer.data.sender
        receiver: account(swap.body.from_user_address)
        dex_incoming_transfer: {
            asset:                     incoming_asset
            amount:                    amount(swap.body.amount)
            source:                    in_transfer.data.sender
            source_jetton_wallet:      in_transfer.data.sender_wallet if in_transfer.data.has_internal_transfer else null
            destination:               account(dex_in_wallet.owner)
            destination_jetton_wallet: account(swap.body.token_wallet)
        }
        dex_outgoing_transfer: {
            asset:                     outgoing_asset
            amount:                    d.out_amount
            source:                    outgoing.data.sender
            source_jetton_wallet:      outgoing.data.sender_wallet
            destination:               out_destination
            destination_jetton_wallet: out_destination_wallet
        }
        destination_asset:  asset(out_wallet.jetton)
        destination_wallet: account(out_wallet_address)
        referral_amount:    d.referral_amount
        referral_address:   d.referral_address
        peer_swaps:          []
        success:             d.success
    }
}
