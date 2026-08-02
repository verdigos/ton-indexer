# Jetton transfer matcher; v0 requires an explicit `entry` capture.
opcode JettonTransfer         = 0x0f8a7ea5
opcode JettonInternalTransfer = 0x178d4519
opcode JettonNotify           = 0x7362d09c

matcher jetton_transfer {
    produces JettonTransferBlock
    build    build_jetton_transfer
    entry    @transfer

    op JettonTransfer @transfer
      -> op JettonInternalTransfer @internal
      -> maybe op JettonNotify @notify
}
