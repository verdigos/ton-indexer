# `msg.opcode` is signed; use the masked `block.data.opcode` for u32 comparisons.
# Missing addresses are addr_none rather than null. worker_payout and grant_refund
# take `failed` from the payout transaction; the other matchers use the anchor.
# cocoon_expected_address preserves external-address formatting. Nested
# nullable parses recover the withdraw amount from the optional refund payload.

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

# The two anchor payloads have identical layouts; only payout_type differs.
matcher cocoon_worker_payout {
    produces cocoon_worker_payout
    entry    @start

    op [CocoonPayoutPayload, CocoonLastPayoutPayload] @start as CocoonLastPayoutPayload | CocoonPayoutPayload
      -> op CocoonWorkerProxyRequest @wreq as CocoonWorkerProxyRequest
         -> { op CocoonPayout @payout
            ; maybe op CocoonReturnExcessesBack @excesses }

    failed when (payout.msg.transaction.aborted ?? false)
    out {
        proxy_contract:   start.msg.source ?? addr_none()
        worker_contract:  start.msg.destination ?? addr_none()
        worker_owner:     account(wreq.body.owner_address) ?? addr_none()
        payout_type:      "last" if start.data.opcode == 0xF5F26A36 else "regular"
        query_id:         start.body.query_id
        new_tokens:       start.body.new_tokens
        expected_address: cocoon_expected_address(start.body.expected_my_address)
        payout_amount:    amount(payout.msg.value)
        worker_state:     wreq.body.state
        worker_tokens:    wreq.body.tokens
    }
}

matcher cocoon_proxy_payout {
    produces cocoon_proxy_payout
    entry    @req

    op CocoonExtProxyPayoutRequest @req as CocoonExtProxyPayoutRequest
      -> { op CocoonPayout @payout
         ; op CocoonReturnExcessesBack @excesses }

    failed when (payout.msg.transaction.aborted ?? false)
    out {
        proxy_contract:     req.msg.destination ?? addr_none()
        proxy_owner:        payout.msg.destination ?? addr_none()
        excesses_recipient: account(req.body.send_excesses_to) ?? addr_none()
        query_id:           req.body.query_id
    }
}

# The usually-aborted ClientProxyRequest child does not determine `failed`.
matcher cocoon_proxy_charge {
    produces cocoon_proxy_charge
    entry    @charge

    op CocoonChargePayload @charge as CocoonChargePayload
      -> { op CocoonReturnExcessesBack @excesses
         ; op CocoonClientProxyRequest @client_request }

    failed when (charge.msg.transaction.aborted ?? false)
    out {
        proxy_contract:   charge.msg.destination ?? addr_none()
        client_contract:  client_request.msg.destination ?? addr_none()
        query_id:         charge.body.query_id
        new_tokens_used:  charge.body.new_tokens_used
        expected_address: cocoon_expected_address(charge.body.expected_my_address)
    }
}

matcher cocoon_client_top_up {
    produces cocoon_client_top_up
    entry    @topup

    op CocoonExtClientTopUp @topup as CocoonExtClientTopUp
      -> op CocoonClientProxyRequest @preq
         -> { op CocoonReturnExcessesBack @excesses }

    failed when (topup.msg.transaction.aborted ?? false)
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
    entry    @reg

    op CocoonRegisterProxy @reg as CocoonRegisterProxy -> op CocoonReturnExcessesBack @excesses

    failed when (reg.msg.transaction.aborted ?? false)
    out {
        root_contract: reg.msg.destination ?? addr_none()
        query_id:      reg.body.query_id
    }
}

matcher cocoon_unregister_proxy {
    produces cocoon_unregister_proxy
    entry    @unreg

    op CocoonUnregisterProxy @unreg as CocoonUnregisterProxy -> op CocoonReturnExcessesBack @excesses

    failed when (unreg.msg.transaction.aborted ?? false)
    out {
        root_contract: unreg.msg.destination ?? addr_none()
        query_id:      unreg.body.query_id
        seqno:         unreg.body.seqno
    }
}

matcher cocoon_client_register {
    produces cocoon_client_register
    entry    @reg

    op CocoonOwnerClientRegister @reg as CocoonOwnerClientRegister
      -> { op CocoonReturnExcessesBack @excesses
         ; op CocoonClientProxyRequest @client_request }

    failed when (reg.msg.transaction.aborted ?? false)
    out {
        client_contract: reg.msg.destination ?? addr_none()
        owner:           account(reg.body.send_excesses_to) ?? addr_none()
        query_id:        reg.body.query_id
        nonce:           reg.body.nonce
    }
}

# new_secret_hash stays an integer; the serializer renders it as bare hex.
matcher cocoon_client_change_secret_hash {
    produces cocoon_client_change_secret_hash
    entry    @chg

    op CocoonOwnerClientChangeSecretHash @chg as CocoonOwnerClientChangeSecretHash
      -> { op CocoonReturnExcessesBack @excesses
         ; op CocoonClientProxyRequest @client_request }

    failed when (chg.msg.transaction.aborted ?? false)
    out {
        client_contract: chg.msg.destination ?? addr_none()
        owner:           account(chg.body.send_excesses_to) ?? addr_none()
        query_id:        chg.body.query_id
        new_secret_hash: chg.body.new_secret_hash
    }
}

# The OwnerWalletSendMessage anchor arm is structurally dead: its only child is
# the refund call, so it cannot satisfy the required children set below.
matcher cocoon_client_request_refund {
    produces cocoon_client_request_refund
    entry    @refund

    op [CocoonOwnerWalletSendMessage, CocoonOwnerClientRequestRefund] @refund as CocoonOwnerClientRequestRefund {
        -> {
            op CocoonReturnExcessesBack @excesses
            ; op CocoonClientProxyRequest @client_request
        }
        <- maybe op CocoonOwnerWalletSendMessage @wallet
    }

    failed when (refund.msg.transaction.aborted ?? false)
    out {
        client_contract: refund.msg.destination ?? addr_none()
        owner:           account(refund.body.send_excesses_to) ?? addr_none()
        query_id:        refund.body.query_id
        via_wallet:      wallet != null
    }
}

matcher cocoon_grant_refund {
    produces cocoon_grant_refund
    entry    @grant

    op CocoonGrantRefundPayload @grant as CocoonGrantRefundPayload
      -> op CocoonClientProxyRequest @client_request
         -> { op CocoonPayout @payout
            ; op CocoonReturnExcessesBack @excesses }

    failed when (payout.msg.transaction.aborted ?? false)
    out {
        proxy_contract:   grant.msg.destination ?? addr_none()
        client_contract:  client_request.msg.destination ?? addr_none()
        refund_recipient: payout.msg.destination ?? addr_none()
        query_id:         grant.body.query_id
        new_tokens_used:  grant.body.new_tokens_used
        expected_address: cocoon_expected_address(grant.body.expected_my_address)
        payout_amount:    amount(payout.msg.value)
    }
}

matcher cocoon_client_increase_stake {
    produces cocoon_client_increase_stake
    entry    @inc

    op CocoonOwnerClientIncreaseStake @inc as CocoonOwnerClientIncreaseStake
      -> { op CocoonReturnExcessesBack @excesses
         ; op CocoonClientProxyRequest @client_request }

    failed when (inc.msg.transaction.aborted ?? false)
    out {
        owner:           account(inc.body.send_excesses_to) ?? addr_none()
        client_contract: inc.msg.destination ?? addr_none()
        query_id:        inc.body.query_id
        new_stake:       amount(inc.body.new_stake)
    }
}

# Only excesses is optional; payout remains required. Nullable nested parses
# keep a malformed or absent refund payload non-fatal.
matcher cocoon_client_withdraw {
    produces cocoon_client_withdraw
    entry    @wd

    op CocoonOwnerClientWithdraw @wd as CocoonOwnerClientWithdraw
      -> op CocoonClientProxyRequest @client_request as CocoonClientProxyRequest
         -> { maybe op CocoonReturnExcessesBack @excesses
            ; op CocoonPayout @payout }

    # A refund grant rides inside the proxy request; anything else leaves the amount at 0.
    let granted = parse? client_request.body.payload as CocoonClientProxyRefundGranted
    failed when (wd.msg.transaction.aborted ?? false)
    out {
        owner:           account(wd.body.send_excesses_to) ?? addr_none()
        client_contract: wd.msg.destination ?? addr_none()
        query_id:        wd.body.query_id
        withdraw_amount: amount(granted.coins ?? 0)
    }
}
