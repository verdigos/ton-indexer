# Matches Cocoon worker, proxy, client, and owner wallet payout, charge, registration, refund, stake, and withdrawal flows.
# Message opcodes are signed, so payout type compares the masked block data opcode.

import "lib/common.mch"

opcode CocoonPayoutPayload              = 0xA040AD28
opcode CocoonLastPayoutPayload          = 0xF5F26A36
opcode CocoonWorkerProxyRequest         = 0x4D725D2C
opcode CocoonPayout                     = 0xC59A7CD3
opcode CocoonReturnExcessesBack         = 0x2565934C
opcode CocoonExtProxyPayoutRequest      = 0x7610E6EB
opcode CocoonChargePayload              = 0xBB63FF93
opcode CocoonClientProxyRequest         = 0x65448FF4
opcode CocoonExtClientTopUp             = 0xF172E6C2
opcode CocoonRegisterProxy              = 0x927C7CB5
opcode CocoonUnregisterProxy            = 0x6D49EAF2
opcode CocoonOwnerClientRegister        = 0xC45F9F3B
opcode CocoonOwnerClientChangeSecretHash = 0xA9357034
opcode CocoonOwnerWalletSendMessage     = 0x9C69F376
opcode CocoonOwnerClientRequestRefund   = 0xFAFA6CC1
opcode CocoonGrantRefundPayload         = 0xEFD711E1
opcode CocoonOwnerClientIncreaseStake   = 0x6A1F6A60
opcode CocoonOwnerClientWithdraw        = 0xDA068E78

matcher cocoon_worker_payout {
    produces cocoon_worker_payout

    op [CocoonPayoutPayload, CocoonLastPayoutPayload] @start as CocoonLastPayoutPayload | CocoonPayoutPayload
      -> op CocoonWorkerProxyRequest @wreq as CocoonWorkerProxyRequest
         -> { op CocoonPayout @payout
            ; maybe op CocoonReturnExcessesBack }

    failed when tx_failed(payout)
    out {
        proxy_contract:   start.msg.source ?? addr_none()
        worker_contract:  start.msg.destination ?? addr_none()
        worker_owner:     account(wreq.body.owner_address) ?? addr_none()
        payout_type:      "last" if start.data.opcode == 0xF5F26A36 else "regular"
        query_id:         start.body.query_id
        new_tokens:       start.body.new_tokens
        expected_address: ext_addr_str(start.body.expected_my_address)
        payout_amount:    amount(payout.msg.value)
        worker_state:     wreq.body.state
        worker_tokens:    wreq.body.tokens
    }
}

matcher cocoon_proxy_payout {
    produces cocoon_proxy_payout

    op CocoonExtProxyPayoutRequest @req as CocoonExtProxyPayoutRequest
      -> { op CocoonPayout @payout
         ; op CocoonReturnExcessesBack }

    failed when tx_failed(payout)
    out {
        proxy_contract:     req.msg.destination ?? addr_none()
        proxy_owner:        payout.msg.destination ?? addr_none()
        excesses_recipient: account(req.body.send_excesses_to) ?? addr_none()
        query_id:           req.body.query_id
    }
}

matcher cocoon_proxy_charge {
    produces cocoon_proxy_charge

    op CocoonChargePayload @charge as CocoonChargePayload
      -> { op CocoonReturnExcessesBack
         ; op CocoonClientProxyRequest @client_request }

    failed when tx_failed(charge)
    out {
        proxy_contract:   charge.msg.destination ?? addr_none()
        client_contract:  client_request.msg.destination ?? addr_none()
        query_id:         charge.body.query_id
        new_tokens_used:  charge.body.new_tokens_used
        expected_address: ext_addr_str(charge.body.expected_my_address)
    }
}

matcher cocoon_client_top_up {
    produces cocoon_client_top_up

    op CocoonExtClientTopUp @topup as CocoonExtClientTopUp
      -> op CocoonClientProxyRequest @preq
         -> { op CocoonReturnExcessesBack }

    failed when tx_failed(topup)
    out {
        client_contract: topup.msg.destination ?? addr_none()
        proxy_contract:  preq.msg.destination ?? addr_none()
        sender:          account(topup.body.send_excesses_to) ?? addr_none()
        query_id:        topup.body.query_id
        top_up_amount:   amount(topup.body.top_up_amount)
    }
}

matcher cocoon_register_proxy {
    produces cocoon_register_proxy

    op CocoonRegisterProxy @reg as CocoonRegisterProxy -> op CocoonReturnExcessesBack

    failed when tx_failed(reg)
    out {
        root_contract: reg.msg.destination ?? addr_none()
        query_id:      reg.body.query_id
    }
}

matcher cocoon_unregister_proxy {
    produces cocoon_unregister_proxy

    op CocoonUnregisterProxy @unreg as CocoonUnregisterProxy -> op CocoonReturnExcessesBack

    failed when tx_failed(unreg)
    out {
        root_contract: unreg.msg.destination ?? addr_none()
        query_id:      unreg.body.query_id
        seqno:         unreg.body.seqno
    }
}

matcher cocoon_client_register {
    produces cocoon_client_register

    op CocoonOwnerClientRegister @reg as CocoonOwnerClientRegister
      -> { op CocoonReturnExcessesBack
         ; op CocoonClientProxyRequest }

    failed when tx_failed(reg)
    out {
        client_contract: reg.msg.destination ?? addr_none()
        owner:           account(reg.body.send_excesses_to) ?? addr_none()
        query_id:        reg.body.query_id
        nonce:           reg.body.nonce
    }
}

matcher cocoon_client_change_secret_hash {
    produces cocoon_client_change_secret_hash

    op CocoonOwnerClientChangeSecretHash @chg as CocoonOwnerClientChangeSecretHash
      -> { op CocoonReturnExcessesBack
         ; op CocoonClientProxyRequest }

    failed when tx_failed(chg)
    out {
        client_contract: chg.msg.destination ?? addr_none()
        owner:           account(chg.body.send_excesses_to) ?? addr_none()
        query_id:        chg.body.query_id
        new_secret_hash: chg.body.new_secret_hash
    }
}

matcher cocoon_client_request_refund {
    produces cocoon_client_request_refund

    op [CocoonOwnerWalletSendMessage, CocoonOwnerClientRequestRefund] @refund as CocoonOwnerClientRequestRefund {
        -> {
            op CocoonReturnExcessesBack
            ; op CocoonClientProxyRequest
        }
        <- maybe op CocoonOwnerWalletSendMessage @wallet
    }

    failed when tx_failed(refund)
    out {
        client_contract: refund.msg.destination ?? addr_none()
        owner:           account(refund.body.send_excesses_to) ?? addr_none()
        query_id:        refund.body.query_id
        via_wallet:      wallet != null
    }
}

matcher cocoon_grant_refund {
    produces cocoon_grant_refund

    op CocoonGrantRefundPayload @grant as CocoonGrantRefundPayload
      -> op CocoonClientProxyRequest @client_request
         -> { op CocoonPayout @payout
            ; op CocoonReturnExcessesBack }

    failed when tx_failed(payout)
    out {
        proxy_contract:   grant.msg.destination ?? addr_none()
        client_contract:  client_request.msg.destination ?? addr_none()
        refund_recipient: payout.msg.destination ?? addr_none()
        query_id:         grant.body.query_id
        new_tokens_used:  grant.body.new_tokens_used
        expected_address: ext_addr_str(grant.body.expected_my_address)
        payout_amount:    amount(payout.msg.value)
    }
}

matcher cocoon_client_increase_stake {
    produces cocoon_client_increase_stake

    op CocoonOwnerClientIncreaseStake @inc as CocoonOwnerClientIncreaseStake
      -> { op CocoonReturnExcessesBack
         ; op CocoonClientProxyRequest }

    failed when tx_failed(inc)
    out {
        owner:           account(inc.body.send_excesses_to) ?? addr_none()
        client_contract: inc.msg.destination ?? addr_none()
        query_id:        inc.body.query_id
        new_stake:       amount(inc.body.new_stake)
    }
}

matcher cocoon_client_withdraw {
    produces cocoon_client_withdraw

    op CocoonOwnerClientWithdraw @wd as CocoonOwnerClientWithdraw
      -> op CocoonClientProxyRequest @client_request as CocoonClientProxyRequest
         -> { maybe op CocoonReturnExcessesBack
            ; op CocoonPayout }

    let granted = parse? client_request.body.payload as CocoonClientProxyRefundGranted
    failed when tx_failed(wd)
    out {
        owner:           account(wd.body.send_excesses_to) ?? addr_none()
        client_contract: wd.msg.destination ?? addr_none()
        query_id:        wd.body.query_id
        withdraw_amount: amount(granted.coins ?? 0)
    }
}
