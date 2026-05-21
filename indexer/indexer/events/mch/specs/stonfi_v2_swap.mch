# Stonfi V2 swap matcher (spec §14.2 adapted: anchor moved to non-recursive prefix)
opcode StonfiV2Swap           = 0x6664de2a
opcode StonfiV2PayTo          = 0x657b54f5
opcode JettonNotify           = 0x7362d09c
opcode PTonTransfer           = 0x01f3835d
opcode JettonInternalTransfer = 0x178d4519

predicate pton_self_transfer

rule next_hops = (
      op JettonInternalTransfer where pton_self_transfer -> op JettonNotify -> maybe $next_hops
    | btype jetton_transfer -> maybe $next_hops
    | maybe $next_hops
)

matcher stonfi_v2_swap {
    produces JettonSwapBlock
    build    build_stonfi_v2_swap
    entry    @swap

    op StonfiV2Swap @swap
      -> op StonfiV2PayTo @payout
      -> $next_hops
}
