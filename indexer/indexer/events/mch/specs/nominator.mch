# Nominator pool withdraw — REMAKE (not a port) of blocks/staking.py
# NominatorPoolWithdrawMatcher (owner-initiated payout, following a
# NominatorPoolProcessWithdrawRequests call). FULLY DECLARATIVE build.
#
# Legacy anchors on the call and loops over every ton_transfer child, building
# ONE NominatorPoolWithdrawRequestBlock per payout while deliberately leaving
# the call itself unconsumed (each payout's `previous_block` stays the call —
# `merge_blocks([transfer_block])` only, never the call).
#
# That fan-out (1 match -> N produced blocks, shared parent unconsumed) is not
# expressible by anchoring on the call. Remake per the port-wave sketch
# ("re-anchor per payout"): anchor directly on each payout `ton_transfer`; the
# parent call is checked via a host predicate on `.previous_block` (never a
# captured node), so it is never part of the consumed set and stays unconsumed
# exactly like legacy. The engine's normal per-block loop then produces one
# independent match per payout for any N, with identical output granularity.
#
# The pool address and the payout amount are both reachable without navigating
# to the (uncaptured) parent: the payout ton_transfer is sent BY the pool, so
# `payout.msg.source` is the pool and `payout.msg.destination` is the nominator;
# the pool block value equals the payout message value (TonTransferBlock.value =
# msg.value), so `payout.msg.value` is the amount. The NominatorPool interface
# check (legacy `get_interfaces` membership, spec §10.7 reject / G10 verdict) is
# a registered nullable lookup keyed on the pool address, rejected when null.

predicate nominator_pool_withdraw_parent

matcher nominator_pool_withdraw {
    produces nominator_pool_withdraw_request
    entry    @payout

    btype ton_transfer @payout where nominator_pool_withdraw_parent

    let pool_iface = lookup nominator_pool(payout.msg.source)
    reject when pool_iface == null
    out {
        source:        account(payout.msg.destination)
        pool:          account(payout.msg.source)
        payout_amount: amount(payout.msg.value)
    }
}
