# Matches Telegram wallet public key rotations from direct external requests and relayed internal requests.

import "lib/common.mch"

opcode TgWalletChangePublicKeyInternal = 0xFBBA99C7
opcode TgWalletChangePublicKeyExternal = 0xFBBA99C8

matcher change_wallet_key {
    produces change_wallet_key

    op [TgWalletChangePublicKeyInternal, TgWalletChangePublicKeyExternal] @req

    failed when tx_failed(req)

    out {
        source:      req.msg.source
        destination: req.msg.destination
        value:       amount(req.msg.value)
    }
}
