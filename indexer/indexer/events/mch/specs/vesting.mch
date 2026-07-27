# Vesting send-message matcher — ported from blocks/vesting.py
# VestingSendMessageBlockMatcher. Linear two-node chain (M1 shape): the
# owner->vesting request plus its required response callback. The response is
# consumed (legacy child_matcher optional=False) but only the request feeds the
# build. VestingAddWhiteListBlockMatcher stays hand-written (out of scope).
#
# Fully declarative build (M4 §9): field-for-field parity with
# build_vesting_send_message_core in blocks/vesting.py. `message_boc_str` uses
# the b64 builtin over the parsed `message_cell` — b64 BOC-encodes a raw Cell
# before base64 (mirrors the legacy `message_cell.to_boc()`).
#
# `success` (upstream #430: the flag is true only when the message was really
# sent) is the one host fn — it scans the request TRANSACTION's out-messages for
# a matching destination + body hash, and transaction-level navigation is
# outside the language (§15b). See builders/vesting.py.

opcode VestingSendMessage         = 0xA7733ACD
opcode VestingSendMessageResponse = 0xF7733ACD

matcher vesting_send_message {
    produces VestingSendMessageBlock
    entry    @send

    op VestingSendMessage @send -> op VestingSendMessageResponse @response

    parse send as VestingSendMessage
    out {
        sender:              account(send.msg.source)
        vesting:             account(send.msg.destination)
        query_id:            send.body.query_id
        message_boc_str:     b64(send.body.message_cell)
        message_destination: account(send.body.message_destination)
        message_value:       amount(send.body.message_value)
        success:             vesting_message_was_sent(send, send.body)
    }
}
