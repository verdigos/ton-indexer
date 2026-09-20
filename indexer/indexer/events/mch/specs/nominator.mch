# Matches nominator pool deposits, withdrawal requests, and withdrawal payouts.

opcode NominatorPoolProcessWithdrawRequests = 0x2

matcher nominator_pool_withdraw {
    produces nominator_pool_withdraw_request

    btype ton_transfer @payout
      <- peek op NominatorPoolProcessWithdrawRequests

    require pool_iface = lookup nominator_pool(payout.msg.source)
    out {
        source:        payout.msg.destination
        pool:          payout.msg.source
        payout_amount: amount(payout.msg.value)
    }
}

matcher nominator_pool_deposit {
    produces        nominator_pool_deposit
    include_excess  false
    include_bounces false

    btype ton_transfer @deposit where (.comment == "d")
      -> { maybe op Excess ; maybe op Bounce }

    require pool_iface = lookup nominator_pool(deposit.msg.destination)
    failed when deposit.failed
    out {
        source: deposit.msg.source
        pool:   deposit.msg.destination
        value:  amount(deposit.msg.value)
    }
}

matcher nominator_pool_withdraw_request {
    produces        nominator_pool_withdraw_request
    include_excess  false
    include_bounces false

    btype ton_transfer @request where (.comment == "w")
      -> {
          maybe op Excess
        ; maybe op Bounce
        ; many[0..2] btype ton_transfer @payouts
      }

    require pool_iface = lookup nominator_pool(request.msg.destination)
    let p = first(payouts)
    failed when request.failed
    out {
        source:        request.msg.source
        pool:          request.msg.destination
        payout_amount: p.data.value if len(payouts) == 2 else (p.data.value if len(payouts) == 1 and p.msg.value > request.msg.value else null)
    }
}
