# Nominator pool withdraw — REMAKE (not a port) of blocks/staking.py
# NominatorPoolWithdrawMatcher (owner-initiated payout, following a
# NominatorPoolProcessWithdrawRequests call).
#
# Legacy anchors on the call and loops over every ton_transfer child, building
# ONE NominatorPoolWithdrawRequestBlock per payout while deliberately leaving
# the call itself unconsumed (each payout's `previous_block` stays the call —
# `merge_blocks([transfer_block])` only, never the call).
#
# That fan-out (1 match -> N produced blocks, shared parent unconsumed) is not
# expressible by anchoring on the call: the compiled build path always does
# `produced.merge_blocks([block] + other_blocks)`, and merging the SAME call
# block into more than one produced block corrupts `Block.parent`/
# `compact_connections` (the second payout's merge steals the call from the
# first). Remake per the port-wave sketch ("re-anchor per payout"): anchor
# directly on each payout `ton_transfer`; the call is checked via a host
# predicate on `.previous_block` (never a captured pattern node), so it is
# never part of the consumed set and stays unconsumed exactly like legacy. The
# engine's normal per-block matching loop then produces one independent match
# per payout for any N, with identical output granularity.
#
# The NominatorPool interface check (async, spec §10.7 reject) lives in the
# shared build core (build_nominator_pool_withdraw_core, blocks/staking.py) —
# same as legacy, just re-run per payout instead of once per call (output
# identical either way: a non-pool call rejects every payout, net result []).

predicate nominator_pool_withdraw_parent

matcher nominator_pool_withdraw {
    produces NominatorPoolWithdrawRequestBlock
    build    build_nominator_pool_withdraw
    entry    @payout

    btype ton_transfer @payout where nominator_pool_withdraw_parent
}
