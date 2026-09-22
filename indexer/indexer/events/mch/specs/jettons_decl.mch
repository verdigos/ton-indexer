# Matches jetton transfers, burns, and mints.

import "lib/common.mch"
import "lib/opcodes.mch"
opcode MinterJettonMint       = 0x00000015

matcher jetton_transfer {
    produces jetton_transfer

    op JettonTransfer @transfer as JettonTransfer
      -> op JettonInternalTransfer @internal as JettonInternalTransfer
      -> maybe op JettonNotify

    require wallet   = lookup jetton_wallet(internal.msg.destination)
    let mismatch = account(wallet.owner) != account(transfer.body.destination)
    let amt      = internal.body.amount ?? transfer.body.amount
    let fwd      = internal.body.forward_ton_amount ?? transfer.body.forward_amount
    failed when any_failed(transfer) or any_failed(internal)
    out {
        has_internal_transfer:  true
        sender:                 transfer.msg.source
        sender_wallet:          transfer.msg.destination
        receiver:               account(wallet.owner) if mismatch else account(transfer.body.destination)
        receiver_wallet:        internal.msg.destination
        response_address:       account(transfer.body.response) ?? addr_none()
        forward_amount:         amount(fwd)
        desired_forward_amount: amount(transfer.body.forward_amount)
        query_id:               transfer.body.query_id
        asset:                  asset(wallet.jetton)
        amount:                 amount(amt)
        desired_amount:         amount(transfer.body.amount)
        forward_payload:        b64(transfer.body.forward_payload)
        custom_payload:         b64(transfer.body.custom_payload)
        comment:                transfer.body.comment
        encrypted_comment:      transfer.body.encrypted_comment
        payload_opcode:         transfer.body.payload_sum_type
        stonfi_swap_body:       transfer.body.stonfi_swap_body
    }
}

matcher jetton_burn {
    produces jetton_burn
    include_bounces false

    op JettonBurn @burn as JettonBurn -> maybe op JettonBurnNotification

    require wallet      = lookup jetton_wallet(burn.msg.destination)
    let burn_amount = burn.body.amount
    failed when any_failed(burn)
    out {
        owner:         account(wallet.owner)
        jetton_wallet: burn.msg.destination
        amount:        amount(burn_amount)
        asset:         asset(wallet.jetton)
    }
}

matcher jetton_mint {
    produces jetton_mint

    op [JettonMint, MinterJettonMint] @mint as MinterJettonMint | JettonMint
      -> maybe or {
                   op JettonNotify
                   op JettonInternalTransfer @internal as JettonInternalTransfer -> maybe op JettonNotify
               }

    let use_internal = not mint.failed and internal != null
    let rjwallet     = lookup jetton_wallet(internal.msg.destination)
    reject when use_internal and rjwallet == null
    failed when any_failed(internal) if use_internal else true
    out {
        to:               account(rjwallet.owner) if use_internal else account(mint.body.to_address)
        to_jetton_wallet: account(rjwallet.address) if use_internal else null
        amount:           amount(internal.body.amount) if use_internal else (amount(mint.body.master_msg_jetton_amount) if mint.msg.opcode == 0x15 else null)
        ton_amount:       amount(mint.body.ton_amount)
        asset:            asset(rjwallet.jetton) if use_internal else asset(mint.msg.destination)
    }
}
