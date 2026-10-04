# Matches Telegram wallet public key rotations from direct external requests and relayed internal requests.
# The wallet logs a successful rotation with an external out message carrying the encrypted old private key.

import "lib/common.mch"

opcode TgWalletChangePublicKeyInternal = 0xFBBA99C7
opcode TgWalletChangePublicKeyExternal = 0xFBBA99C8
opcode TgWalletKeyChangedLog = 0xEBA19948

matcher change_wallet_key {
    produces change_wallet_key

    op [TgWalletChangePublicKeyInternal, TgWalletChangePublicKeyExternal] @req as SignedChangePublicKeyRequest
        -> op TgWalletKeyChangedLog @log as OnKeyHasBeenChangedEvent

    reject when (req.msg.source == null) != (req.data.opcode == 0xFBBA99C8)
    reject when tx_failed(req)
    # The two Telegram wallet code versions.
    let code = lookup code_hash(req.msg.destination)
    reject when code != "kUmuUcHkaJcQzr94MCl7Fqz7rbNjqSClN4k+f/7sp2g=" and code != "4wkRQgvvEZHAnc5Yud8rTKTC2cODzDtqkRcDSf+nDiw="
    require request = req.body.request
    require key_log = log.body

    out {
        source:                    req.msg.source
        destination:               req.msg.destination
        value:                     amount(req.msg.value)
        new_public_key:            hex(request.newPublicKey, 32)
        rotation_signature:        hex(request.rotationSignature.ref, 64)
        encrypted_old_private_key: hex(key_log.encryptedOldPrivateKey, 32)
    }
}
