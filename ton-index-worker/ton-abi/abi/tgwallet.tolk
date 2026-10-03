tolk 1.4

// Key rotation declarations from the official Telegram wallet (WalletTg) contract.

struct SignedRequest<T> {
    signature: bits512
    request: T
}

struct SeqnoHeader {
    subwalletId: uint32
    validUntil: uint32
    seqno: uint32
}

struct (0xFBBA99C7) ChangePublicKeyRequestI {
    header: SeqnoHeader
    newPublicKey: uint256
    rotationSignature: Cell<bits512>
    encryptedOldPrivateKey: Cell<bits256>
}

struct (0xFBBA99C8) ChangePublicKeyRequestE {
    header: SeqnoHeader
    newPublicKey: uint256
    rotationSignature: Cell<bits512>
    encryptedOldPrivateKey: Cell<bits256>
}

struct (0xEBA19948) OnKeyHasBeenChangedEvent {
    encryptedOldPrivateKey: bits256
}

type SignedChangePublicKeyRequest = SignedRequest<ChangePublicKeyRequestI | ChangePublicKeyRequestE>

contract WalletTg {
    incomingMessages: SignedChangePublicKeyRequest
    forceAbiExport: OnKeyHasBeenChangedEvent
}
