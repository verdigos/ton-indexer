# Dedust swap with a declarative build and host functions for the remaining fields.
#
# Pattern is identical to specs/dedust_swap.mch. The build replaces the
# monolithic `build build_dedust_swap` directive with build statements:
# expressions own the reject/failed wiring and every scalar field they can
# genuinely derive; two registered host fns own the imperative residue.
#
# Honest split (field-by-field):
#   declarative — dex (literal); sender and source_asset for the TON-in branch
#     (envelope access on the raw @swap_request call + ton_asset()); the
#     reject/failed decisions themselves.
#   imperative (host fns) — dedust_swap_legs: peer-swap list assembly (scan +
#     lt-sort over the consumed set: recursion exit blocks carry no captures),
#     jetton-branch in-leg (needs jetton_transfer block DATA — composed blocks
#     have no reliable `.msg`, their event-node order is set-shuffled by
#     merge_blocks), payout resolution (next_blocks/include membership walk),
#     destination_asset (BOC steps walk + async pool-hop loop), and the nested
#     transfer/leg dicts (no record literals in the language);
#     dedust_swap_failed: notification-coverage scan (no list quantifiers).
#
# Known duplication forced by the missing record literals: the fn embeds
# sender/asset_in inside the nested dicts it must build anyway, so the
# declarative TON-branch arms re-derive values the fn also computes.

opcode DedustSwapExternal     = 0x61ee542d
opcode DedustSwapPeer         = 0x72aca8aa
opcode DedustSwap             = 0xea06185d
opcode DedustSwapNotification = 0x9c610de3
opcode DedustPayoutFromPool   = 0xad4eb6f5
opcode DedustPayout           = 0x474f86cf

# One pool hop per unrolling: a peer (or nested external) swap request with an
# optional notification child; exit = the terminal payout chain.
rule peer_chain = (
    op DedustSwapPeer @peer
      -> { maybe op DedustSwapNotification @peer_notification }
      -> maybe $peer_chain
  ^ op DedustSwapExternal @peer
      -> { maybe op DedustSwapNotification @peer_notification }
      -> maybe $peer_chain
  ^ op DedustPayoutFromPool -> ( op DedustPayout | btype jetton_transfer )
)

matcher dedust_swap_hybrid {
    produces JettonSwapBlock
    entry    @ext

    op DedustSwapExternal @ext
      -> {
           ( op DedustPayoutFromPool @payout
               -> ( btype jetton_transfer @out_jetton | op DedustPayout @out_ton )
           | $peer_chain )
           maybe op DedustSwapNotification @notification
         }
      <- ( btype jetton_transfer @in_transfer | op DedustSwap @swap_request )

    let legs = dedust_swap_legs(consumed, in_transfer, swap_request)
    reject when legs == null
    failed when dedust_swap_failed(consumed)
    out {
        dex:                   "dedust"
        sender:                account(swap_request.msg.source) if in_transfer == null else legs.sender
        source_asset:          ton_asset() if in_transfer == null else legs.asset_in
        destination_asset:     legs.destination_asset
        dex_incoming_transfer: legs.dex_incoming_transfer
        dex_outgoing_transfer: legs.dex_outgoing_transfer
        in:                    legs.in_leg
        out:                   legs.out_leg
        peer_swaps:            legs.peer_swaps
    }
}
