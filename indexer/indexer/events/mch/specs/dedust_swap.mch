# Dedust swap (T15) — ported from blocks/swaps.py DedustSwapBlockMatcher.
#
# Production reality mirrored here: DedustPeerBlockMatcher is defined in
# swaps.py but never registered in event_processing.matchers, so the recursive
# chain sees raw DedustSwapPeer/DedustSwapExternal opcodes (never
# SingleLevelWrapper blocks).
#
# Capture notes (T15 gaps resolved by T18, spec §7.2):
# - captures in $peer_chain are list bindings; since T18 exit-branch captures
#   bind once on termination and post-recursion captures (@notification,
#   @in_transfer/@swap_request) bind on multi-hop too. The exit payout chain
#   is left uncaptured for historical fidelity; the builder still works off
#   the consumed set (Match._mch_consumed), like the legacy build_block works
#   off other_blocks — the simplest faithful input for its scan logic.

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
