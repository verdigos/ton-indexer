# Stonfi V2 swap matcher corresponding to StonfiV2SwapBlockMatcher.
#
# Implementation-specific shape details:
# - anchor moved out of the recursive rule (entry cannot be a list binding);
#   the first hop's swap/payout/out-alternative live in the matcher pattern,
#   hops 2..n repeat via $swap_hop;
# - the intermediate pTON arm requires a non-internal jetton transfer, a
#   PTonTransfer child, and a forward payload carrying the V2 swap tag;
# - overlapping hop branches use `or` and resolve first-match-in-order;
# - terminal-pton branch carries `{ maybe op PTonTransfer }` so the pattern
#   consumes the pton call the legacy build added via
#   additional_blocks_to_include.
#
# Captures are kept for pattern fidelity but the builder works off the
# consumed set (Match._mch_consumed). It stays consumed-set-driven because the
# legacy scan logic needs the full set
# anyway (same situation as dedust_swap.mch).

import "lib/opcodes.mch"
opcode StonfiV2Swap  = 0x6664de2a

# One router hop per unrolling: swap request + payment, then continuation via
# a validated intermediate pton chain, a plain jetton transfer, or directly.
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

# HYBRID declarative build. The pattern, the failed flag and the out-field
# mapping are declarative; the imperative residue stays in one named host fn
# (a gap, like dedust_swap_legs):
#   stonfi_v2_swap_data — reconstructs the (swap, pay_to) leg pairs and the
#     in/out transfers from the consumed set, then delegates to the shared
#     build_stonfi_v2_swap_core (async get_pool_accounts_recursive walk,
#     per-hop get_jetton_wallet lookups, peer-leg list assembly, the broken-
#     loop wallet-miss guard, and the failed-swap destination_asset fallback).
#     Returns a namespace mirroring the core's data dict; `.failed` drives the
#     failed flag.
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
