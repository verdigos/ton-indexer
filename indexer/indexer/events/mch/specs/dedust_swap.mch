# Dedust swap (T15) — ported from blocks/swaps.py DedustSwapBlockMatcher.
#
# Production reality mirrored here: DedustPeerBlockMatcher is defined in
# swaps.py but never registered in event_processing.matchers, so the recursive
# chain sees raw DedustSwapPeer/DedustSwapExternal opcodes (never
# SingleLevelWrapper blocks).
#
# Capture caveats (see roadmap T15 notes):
# - captures in $peer_chain are list bindings (§7.2); the exit payout chain is
#   left uncaptured because exit-branch captures never bind at runtime;
# - @payout/@out_jetton/@out_ton/@notification/@in_transfer/@swap_request bind
#   reliably only on the single-hop path — after a recursion the walker cannot
#   bind later siblings. The builder therefore works off the consumed set
#   (Match._mch_consumed), like the legacy build_block works off other_blocks.

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

matcher dedust_swap {
    produces JettonSwapBlock
    build    build_dedust_swap
    entry    @ext

    op DedustSwapExternal @ext
      -> {
           ( op DedustPayoutFromPool @payout
               -> ( btype jetton_transfer @out_jetton | op DedustPayout @out_ton )
           | $peer_chain )
           maybe op DedustSwapNotification @notification
         }
      <- ( btype jetton_transfer @in_transfer | op DedustSwap @swap_request )
}
