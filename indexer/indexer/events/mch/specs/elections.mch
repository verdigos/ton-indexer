# Matches stake deposits to and recoveries from the masterchain elector account.

opcode ElectorDepositStakeRequest      = 0x4E73744B
opcode ElectorDepositStakeConfirmation = 0xF374484C
opcode ElectorRecoverStakeRequest      = 0x47657424
opcode ElectorRecoverStakeConfirmation = 0xF96F7324

matcher election_deposit {
    produces        election_deposit
    include_excess  false

    op ElectorDepositStakeRequest @req -> maybe op ElectorDepositStakeConfirmation @conf

    reject when req.msg.destination != "-1:3333333333333333333333333333333333333333333333333333333333333333"
    failed when conf == null

    out {
        amount:       amount(req.msg.value)
        stake_holder: req.msg.source
    }
}

matcher election_recover {
    produces        election_recover
    include_excess  false

    op ElectorRecoverStakeRequest @req -> maybe op ElectorRecoverStakeConfirmation @conf

    reject when req.msg.destination != "-1:3333333333333333333333333333333333333333333333333333333333333333"
    failed when conf == null

    out {
        amount?:      amount(conf.msg.value)
        stake_holder: req.msg.source
    }
}
