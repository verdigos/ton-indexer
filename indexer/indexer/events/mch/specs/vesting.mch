# Matches vesting wallet sends and whitelist additions with their required response callbacks.

opcode VestingSendMessage         = 0xA7733ACD
opcode VestingSendMessageResponse = 0xF7733ACD

matcher vesting_send_message {
    produces vesting_send_message

    op VestingSendMessage @send as VestingSendMessage -> op VestingSendMessageResponse

    reject when lookup code_hash(send.msg.destination) != "tItTGr7DtxRjgpH3137W3J9qJynvyiBHcTc3TUrotZA="
    out {
        sender:              send.msg.source
        vesting:             send.msg.destination
        query_id:            send.body.query_id
        message_boc_str:     b64(send.body.message_cell)
        message_destination: account(send.body.message_destination)
        message_value:       amount(send.body.message_value)
        success:             any(out_messages(send) as m =>
                                 account(m.destination) == account(send.body.message_destination)
                                 and send.body.message_body_hash != null
                                 and m.body_hash == send.body.message_body_hash)
    }
}

opcode VestingAddWhiteList         = 0x7258A69B
opcode VestingAddWhiteListResponse = 0xF258A69B

matcher vesting_add_whitelist {
    produces vesting_add_whitelist

    op VestingAddWhiteList @add as VestingAddWhiteList -> op VestingAddWhiteListResponse

    reject when lookup code_hash(add.msg.destination) != "tItTGr7DtxRjgpH3137W3J9qJynvyiBHcTc3TUrotZA="
    out {
        adder:          add.msg.source
        vesting:        add.msg.destination
        query_id:       add.body.query_id
        accounts_added: map(add.body.addresses as a => account(a) ?? addr_none())
    }
}
