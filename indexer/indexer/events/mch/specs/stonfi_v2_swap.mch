# Matches STON.fi V2 swaps across chained router hops and pTON or jetton payout legs.

import "lib/opcodes.mch"
opcode StonfiV2Swap  = 0x6664de2a

rule swap_hop = or {
                    op StonfiV2Swap
                      -> op StonfiV2PayTo
                      -> btype jetton_transfer where (not .has_internal_transfer)
                      -> op PTonTransfer where ((parse? tail_unwrap((parse? . as PTonTransfer).forward_payload) as StonfiV2SwapPayloadTag) != null)
                      -> op JettonNotify
                      -> maybe $swap_hop
                    op StonfiV2Swap
                      -> op StonfiV2PayTo
                      -> btype jetton_transfer {
                          -> { maybe op PTonTransfer }
                          -> maybe $swap_hop
                      }
                    op StonfiV2Swap
                      -> op StonfiV2PayTo
                      -> maybe $swap_hop
                }

matcher stonfi_v2_swap {
    produces jetton_swap

    op StonfiV2Swap @swap {
        -> {
            op StonfiV2PayTo
                -> or {
                       btype jetton_transfer where (not .has_internal_transfer)
                          -> op PTonTransfer where ((parse? tail_unwrap((parse? . as PTonTransfer).forward_payload) as StonfiV2SwapPayloadTag) != null)
                          -> op JettonNotify
                          -> maybe $swap_hop
                       btype jetton_transfer {
                          -> { maybe op PTonTransfer }
                          -> maybe $swap_hop
                       }
                       $swap_hop
                   }
        }
        <- xor {
               btype jetton_transfer
               op JettonNotify <- op PTonTransfer
           }
    }

    let d = stonfi_v2_swap_data(consumed)
    failed when d.failed
    out {
        dex:                   d.dex
        source_asset:          d.source_asset
        destination_asset:     d.destination_asset
        sender:                d.sender
        dex_incoming_transfer: d.dex_incoming_transfer
        dex_outgoing_transfer: d.dex_outgoing_transfer
        referral_amount:       d.referral_amount
        referral_address:      d.referral_address
        peer_swaps:            d.peer_swaps
    }
}
