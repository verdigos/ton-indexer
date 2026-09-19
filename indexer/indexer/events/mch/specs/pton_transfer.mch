# pTON transfer matcher — port of blocks/jettons.py PTonTransferMatcher.
#
# A pTON ("proxy TON") wallet wraps native TON behind the jetton transfer
# interface: it accepts the ordinary 0x0f8a7ea5 body but never emits an internal
# transfer, so `jetton_transfer` (specs/jettons_decl.mch, whose pattern requires
# the internal leg) can never match one. Without this matcher the pTON leg of a
# DEX swap stays an unconsumed call_contract and every consumer that binds
# `btype jetton_transfer` — stonfi v1/v2, dedust, tonco — loses its TON-side leg.
#
# The manifest puts pton_transfer after jetton_transfer on their shared anchor,
# and before the swap matchers that consume their blocks.
#
# The host's TON-proxy master allowlist (STON.fi pTON v1/v2, Tonco v1 wTTON,
# Tonco v2 TON wrapper) makes the match safe on an opcode this common: only a
# wallet whose master is one of the canonical addresses qualifies.
#
# Field parity with the legacy build: `has_internal_transfer` is false and both
# wallet fields are null (there is no internal leg to name them from — the flag
# is what the stonfi v2 pattern keys its pton arms on), and the legacy build sets
# no `desired_amount`/`desired_forward_amount` here, so neither does this.

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
