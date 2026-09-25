# Matches pTON transfers that use the jetton transfer body without an internal transfer.

import "lib/opcodes.mch"

matcher pton_transfer {
    produces jetton_transfer

    op JettonTransfer @transfer as JettonTransfer -> maybe op JettonNotify

    require wallet = lookup jetton_wallet(transfer.msg.destination)
    reject when not is_pton_master(wallet.jetton)
    failed when transfer.failed
    out {
        has_internal_transfer: false
        sender:                transfer.msg.source
        sender_wallet:         null
        receiver:              account(transfer.body.destination)
        receiver_wallet:       null
        response_address:      account(transfer.body.response) ?? addr_none()
        forward_amount:        amount(transfer.body.forward_amount)
        query_id:              transfer.body.query_id
        asset:                 asset(wallet.jetton)
        amount:                amount(transfer.body.amount)
        forward_payload:       b64(transfer.body.forward_payload)
        custom_payload:        b64(transfer.body.custom_payload)
        comment:               transfer.body.comment
        encrypted_comment:     transfer.body.encrypted_comment
        payload_opcode:        transfer.body.payload_sum_type
        stonfi_swap_body:      transfer.body.stonfi_swap_body
    }
}
