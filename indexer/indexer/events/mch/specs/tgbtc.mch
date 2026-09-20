# tgBTC matchers cover mint, burn, new key, and standalone log flows.
import "lib/common.mch"

opcode TgBTCMintHead       = 0x3F781D24
opcode TgBTCMintTail       = 0xE42A879B
opcode TgBTCMintSuccessLog = 0x77A80EF3

opcode JettonBurnNotify     = 0x7BDD97DE
opcode TgBTCBurnNotify      = 0x587643A2
opcode TgBTCNewKeyRequest   = 0x690F357A
opcode TgBTCPayout          = 0xBE44E7A6
opcode TgBTCPayoutNotify    = 0x1A84C0E0
opcode TgBTCBurnLog         = 0xCA444CE6
opcode TgBTCNewKeyLog       = 0x27756729
opcode TgBTCDkgCompletedLog = 0x453443A6

matcher tgbtc_mint {
    produces tgbtc_mint
    shape    tgbtc_deploy_absorb

    op TgBTCMintHead @head -> $mint_chain
    let mint_head = find(consumed as b => b.btype == "call_contract" and b.data.opcode == 0x3F781D24)
    let mint_log = find(consumed as b => b.btype == "call_contract" and b.data.opcode == 0x77A80EF3)
    let mint_jetton = find(consumed as b => b.btype == "jetton_mint")
    require d = tgbtc_mint_data(mint_head, mint_log, mint_jetton)
    out {
        sender:            head.msg.source
        recipient:         d.recipient
        amount:            d.amount
        asset:             d.asset
        bitcoin_txid:      d.bitcoin_txid
        recipient_wallet:  d.recipient_wallet
        teleport_contract: d.teleport_contract
        crippled:          false
    }
}

rule mint_chain = or {
                      any -> maybe $mint_chain
                      op TgBTCMintTail
                        -> { op TgBTCMintSuccessLog ; btype jetton_mint }
                  }

matcher tgbtc_burn {
    produces tgbtc_burn

    btype jetton_burn @burn
      -> op [JettonBurnNotify, TgBTCBurnNotify]
           -> { op TgBTCBurnLog @log as TgBTCBurnEvent
              ; maybe op TgBTCPayout
                  -> { maybe op TgBTCPayoutNotify ; maybe btype ton_transfer } }

    require b = log.body
    let chain_amount = burn.data.amount.value
    let use_log      = (chain_amount ?? 0) == 0
    let min_amount   = min(chain_amount, b.amount)
    out {
        sender:         burn.data.owner
        jetton_wallet:  burn.data.jetton_wallet
        asset:          burn.data.asset
        amount:         amount(b.amount if use_log else min_amount)
        pegout_address: b.pegout_address
        crippled:       false
    }
}

matcher tgbtc_new_key {
    produces tgbtc_new_key

    op TgBTCNewKeyRequest @k {
        -> {
            op TgBTCNewKeyLog @log as TgBTCNewKeyEvent
            ; maybe op TgBTCPayout
                -> { maybe op TgBTCPayoutNotify ; maybe btype ton_transfer }
        }
        <- any -> { op TgBTCDkgCompletedLog @dkg as TgBTCDkgCompletedEvent }
    }

    require n = log.body
    require g = dkg.body
    out {
        coordinator_contract: log.msg.source
        pubkey:               uint_hex(n.new_internal_pubkey)
        pegout_address:       n.pegout_address
        amount:               n.amount
        teleport_contract:    dkg.msg.source
        timestamp:            g.timestamp
        crippled:             false
    }
}

matcher tgbtc_mint_log_only {
    produces tgbtc_mint

    op TgBTCMintSuccessLog @log as TgBTCMintEvent

    require m = log.body
    out {
        sender:            log.msg.source
        recipient:         m.recipient_address
        amount:            m.amount
        asset:             null
        bitcoin_txid:      txid_le_hex(m.bitcoin_txid)
        success:           true
        recipient_wallet:  null
        teleport_contract: log.msg.source
        crippled:          true
    }
}

matcher tgbtc_burn_log_only {
    produces tgbtc_burn

    op TgBTCBurnLog @log as TgBTCBurnEvent

    require b = log.body
    out {
        sender:         b.sender_address
        jetton_wallet:  null
        asset:          null
        amount:         amount(b.amount)
        pegout_address: b.pegout_address
        crippled:       true
    }
}

matcher tgbtc_new_key_log_only {
    produces tgbtc_new_key

    op TgBTCNewKeyLog @log as TgBTCNewKeyEvent

    require n = log.body
    out {
        coordinator_contract: log.msg.source
        pubkey:               uint_hex(n.new_internal_pubkey)
        pegout_address:       n.pegout_address
        amount:               n.amount
        teleport_contract:    null
        timestamp:            null
        crippled:             true
    }
}

matcher tgbtc_dkg_log_only {
    produces tgbtc_dkg_log

    op TgBTCDkgCompletedLog @log as TgBTCDkgCompletedEvent

    require g = log.body
    out {
        coordinator_contract: log.msg.source
        timestamp:            g.timestamp
        internal_pubkey:      uint_hex(g.internal_pubkey)
    }
}
