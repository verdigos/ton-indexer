# Wallet plugin subscriptions cover recurring payments and plugin destruction.

opcode Request              = 0x706C7567
opcode Response             = 0xF06C7567
opcode Payment              = 0x73756273
opcode WalletPluginDestruct = 0x64737472

matcher subscribe {
    produces subscribe

    op Response @resp {
        -> { op Payment @pay }
        <- maybe op Request @req as SubscriptionPaymentRequest
    }

    failed when req != null and req.failed
    out {
        subscriber:   resp.msg.source
        subscription: resp.msg.destination
        beneficiary:  pay.msg.destination
        amount:       amount(req.body.grams) if req != null else amount(resp.msg.value)
    }
}

matcher unsubscribe {
    produces unsubscribe

    op WalletPluginDestruct @destruct -> maybe op WalletPluginDestruct @response

    out {
        subscriber:   destruct.msg.source
        subscription: destruct.msg.destination
        beneficiary:  response.msg.destination
    }
}
