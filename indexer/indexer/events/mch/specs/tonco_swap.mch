# Matches Tonco swaps across one or more router hops to a jetton or pTON payout.

import "lib/opcodes.mch"
opcode ToncoPoolV3Swap    = 0xa7fb58f8

rule tonco_hop cyclic = (
    op ToncoPoolV3Swap
      -> op ToncoRouterV3PayTo
      -> or {
             btype jetton_transfer -> op PTonTransfer -> op JettonNotify -> $tonco_hop
             btype jetton_transfer -> $tonco_hop
             op JettonTransfer -> op PTonTransfer
             btype jetton_transfer -> { maybe op PTonTransfer }
         }
)

matcher tonco_swap {
    produces jetton_swap

    op ToncoPoolV3Swap @swap {
        -> {
            op ToncoRouterV3PayTo
                -> or {
                       btype jetton_transfer -> op PTonTransfer -> op JettonNotify -> $tonco_hop
                       btype jetton_transfer -> $tonco_hop
                       op JettonTransfer -> op PTonTransfer
                       btype jetton_transfer -> { maybe op PTonTransfer }
                   }
        }
        <- xor {
               btype jetton_transfer
               op JettonNotify <- op PTonTransfer
           }
    }

    require r = tonco_swap_data(consumed)
    failed when r.failed
    out {
        dex:                   r.dex
        source_asset:          r.source_asset
        destination_asset:     r.destination_asset
        sender:                r.sender
        dex_incoming_transfer: r.dex_incoming_transfer
        dex_outgoing_transfer: r.dex_outgoing_transfer
        referral_amount:       r.referral_amount
        referral_address:      r.referral_address
        peer_swaps:            r.peer_swaps
        min_out_amount:        r.min_out_amount
    }
}
