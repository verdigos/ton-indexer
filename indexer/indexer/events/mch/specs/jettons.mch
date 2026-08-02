# Jetton family matchers corresponding to blocks/jettons.py.
# PTonTransferMatcher and FallbackJettonTransferBlockMatcher stay hand-written
# (build-side disambiguation in the declarative form).

opcode JettonTransfer         = 0x0f8a7ea5
opcode JettonInternalTransfer = 0x178d4519
opcode JettonNotify           = 0x7362d09c
opcode JettonBurn             = 0x595f07bc
opcode JettonBurnNotification = 0x7bdd97de
opcode JettonMint             = 0x642b7d07
opcode MinterJettonMint       = 0x00000015

matcher jetton_transfer {
    produces JettonTransferBlock
    build    build_jetton_transfer
    entry    @transfer

    op JettonTransfer @transfer
      -> op JettonInternalTransfer @internal
      -> maybe op JettonNotify @notify
}

matcher jetton_burn {
    produces JettonBurnBlock
    build    build_jetton_burn
    entry    @burn

    op JettonBurn @burn -> maybe op JettonBurnNotification @notification
}

matcher jetton_mint {
    produces JettonMintBlock
    build    build_jetton_mint
    entry    @mint

    (op JettonMint @mint ^ op MinterJettonMint @mint)
      -> maybe ( op JettonNotify @notify
               | op JettonInternalTransfer @internal -> maybe op JettonNotify @internal_notify )
}
