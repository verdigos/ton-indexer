# Coffee swap (T17) — ported from blocks/swaps.py CoffeeSwapBlockMatcher.
#
# New shapes vs earlier ports (jettons/dns/dedust_swap):
# - recursion step is a two-node CHAIN (`op CoffeeSwapInternal -> op
#   CoffeeSwapSuccessfulEvent`): each unrolling consumes the swap AND its
#   success event, mirroring legacy RecursiveMatcher(repeating=ContractMatcher(
#   SwapInternal, child=ContractMatcher(SuccessEvent)));
# - same-name capture across exclusive parent branches (@in_transfer, T02) —
#   first port to use it;
# - three-way mixed op/btype alternative under the payout exit.
#
# The anchor children set mirrors legacy children_matchers=[event,
# Or([recursion, payout_chain])]: the payout chain appears BOTH as the rule's
# exit branch (multi-hop) and as the direct Or branch (zero intermediate
# hops). Capture caveats (same as dedust_swap, see roadmap T15 notes):
# @payout/@out/@in_transfer bind only on the zero-hop path — after a recursion
# the walker cannot bind later siblings, and exit-branch captures never bind.
# The builder therefore works off the consumed set (Match._mch_consumed);
# captures are kept for pattern fidelity.

opcode CoffeeSwapNative          = 0xc0ffee00
opcode CoffeeSwapInternal        = 0xc0ffee20
opcode CoffeeSwapSuccessfulEvent = 0xc0ffee30
opcode CoffeePayoutInternal      = 0xc0ffee21
opcode CoffeePayout              = 0xc0ffee32
opcode CoffeeNotification        = 0xc0ffee36

# One pool hop per unrolling: a swap request with its success event; exit =
# the payout chain to the user.
rule swap_chain = (
    op CoffeeSwapInternal @peer -> op CoffeeSwapSuccessfulEvent @peer_event -> maybe $swap_chain
  ^ op CoffeePayoutInternal -> ( btype jetton_transfer | op CoffeePayout | op CoffeeNotification )
)

matcher coffee_swap {
    produces JettonSwapBlock
    build    build_coffee_swap
    entry    @swap

    op CoffeeSwapInternal @swap
      -> { op CoffeeSwapSuccessfulEvent @event
         ; ( $swap_chain
           | op CoffeePayoutInternal @payout
               -> ( btype jetton_transfer @out ^ op CoffeePayout @out ^ op CoffeeNotification @out ) ) }
      <- ( btype jetton_transfer @in_transfer ^ op CoffeeSwapNative @in_transfer )
}
