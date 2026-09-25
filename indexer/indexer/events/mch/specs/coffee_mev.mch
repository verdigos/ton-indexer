# Coffee MEV matchers cover hold-funds and failed-swap flows in TON and jetton forms.

opcode CoffeeMevProtectHoldFunds = 0x6BC79E7E
opcode CoffeeMevProtectFailedSwap = 0xEE51CE51
opcode CoffeeServiceFee          = 0xC0FFEEA0

matcher coffee_mev_protect_hold_funds {
    produces coffee_mev_protect_hold_funds

    op CoffeeMevProtectHoldFunds @call
      <- maybe ( any -> { op CoffeeServiceFee } )

    out {
        sender:              call.msg.source
        mev_contract:        call.msg.destination
        asset:               ton_asset()
        sender_wallet:       addr_none()
        mev_contract_wallet: null
        amount:              amount(call.msg.value)
    }
}

matcher coffee_mev_protect_hold_funds_jetton {
    produces coffee_mev_protect_hold_funds

    btype jetton_transfer @transfer where (.payload_opcode == 0x6bc79e7e)
      <- maybe ( any -> { op CoffeeServiceFee } )

    out {
        sender:              transfer.data.sender
        mev_contract:        transfer.data.receiver
        asset:               transfer.data.asset
        sender_wallet:       transfer.data.sender_wallet
        mev_contract_wallet: null
        amount:              transfer.data.amount
    }
}

matcher coffee_mev_protect_failed_swap {
    produces coffee_mev_protect_failed_swap

    op CoffeeMevProtectFailedSwap @call

    out {
        recipient: (parse! call as CoffeeMevProtectFailedSwap).recipient
        asset:     ton_asset()
    }
}

matcher coffee_mev_protect_failed_swap_jetton {
    produces coffee_mev_protect_failed_swap

    btype jetton_transfer @transfer where (.payload_opcode == 0xee51ce51)

    out {
        recipient: (parse! transfer.data.forward_payload as CoffeeMevProtectFailedSwap).recipient
        asset:     transfer.data.asset
    }
}
