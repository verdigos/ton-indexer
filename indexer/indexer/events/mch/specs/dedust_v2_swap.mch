# DeDust CPMM V2 swap corresponding to DedustV2SwapBlockMatcher and
# DedustV2FailedSwapBlockMatcher (blocks/dedust_v2/swaps.py).
#
# Route shape (one unrolling of $dv2_hop per pool hop):
#
#   carrier (PayNative 0xa5a7cbf8 | jetton_transfer into the pool)
#     Swap event 0x78e79ba4                # the pool's ext-out log: the ONLY
#     continuation                         # exact source of amount_in/amount_out
#       = next carrier (another hop)  |  out leg (jetton_transfer | Payout 0x3216ca09)
#     excess 0xd53276db
#
# The reference matcher takes both carrier kinds in test_self; here that is one
# anchor XOR mixes an `op` and a `btype` head under one capture, so a
# single matcher covers TON-in and jetton-in.
#
# Recursion notes (the shape is forced by the frontier walk):
# - the out leg appears TWICE: as the rule's exit arms (routes WITH hops, where
#   it is reached on the post-step frontier) and again at the invocation site
#   (single-hop routes, where the walk never steps; the frontier entry gate is
#   the STEP head, so an exit-only recursion can never fire);
# - the direct integer `payload_opcode` comparisons keep the exit-tried-first
#   order from mistaking an intermediate jetton carrier for the out leg (the
#   premature exit: a pool-to-pool carrier rides a PayJetton
#   (0xcbc33949) forward payload, the user-facing out leg does not;
# - `maybe $dv2_hop` hangs off the Swap EVENT, not off the carrier's children
#   set, and that position is load-bearing. An in-step self-reference lowers to
#   an `any` node that CONSUMES a candidate, so under the carrier it would eat
#   the next hop's carrier before the frontier reaches it. A hop head that
#   is already consumed contributes no next_blocks to the following frontier,
#   which truncated every 3-hop route to its last two hops. The event is a
#   childless ext-out log, so the placeholder finds no candidate and the walk
#   is driven purely by the frontier. R006 only asks that a maybe-guarded self
#   reference exist.
#
# Per-hop amounts come from the Swap events and per-hop assets from the
# carriers, so the peer-swap list is expressible without a host fn: hop i swaps
# in_assets[i] -> leg_assets[i], where leg_assets is [hop carriers…, out leg]
# and in_assets is that sequence shifted by the entry carrier. The reference
# builds the same pairing by scanning the consumed set and sorting on min_lt;
# on a linear route (a pool forwards to exactly one continuation) the two agree.
#
# `parse?` unwraps the PayJetton wrapper and then validates the payload.
# The loop-only min-out helper walks the payload's `next` SwapStep chain to its
# final minimal_amount_out.
#
# `out` keys are the reference matcher's JettonSwapBlock data keys, so the same
# serializer fill (_fill_jetton_swap_action) serves both builds.

import "lib/common.mch"
import "lib/leg.mch"

opcode DedustV2PayNative = 0xa5a7cbf8
opcode DedustV2SwapEvent = 0x78e79ba4
opcode DedustV2Payout    = 0x3216ca09
# One pool hop per unrolling (step arms), then the terminal out leg (exit arms).
rule dv2_hop = or {
                   op DedustV2PayNative @hop
                     -> { op DedustV2SwapEvent @ev -> maybe $dv2_hop }
                   btype jetton_transfer @hop where (.payload_opcode == 0xcbc33949)
                     -> { op DedustV2SwapEvent @ev -> maybe $dv2_hop }
                   btype jetton_transfer @exit_leg where (.payload_opcode != 0xcbc33949)
                   op DedustV2Payout @exit_leg
               }

matcher dedust_v2_swap {
    produces JettonSwapBlock
    entry    @carrier
    include_excess false          # the reference consumes the carrier's excess
                                  # INTO the swap block (explicit child below),
                                  # not as a sibling of the result

    xor {
        op DedustV2PayNative @carrier as DedustV2PayNative
        btype jetton_transfer @carrier
    }
      -> { op DedustV2SwapEvent @ev0 as DedustV2SwapEvent
           or {
               $dv2_hop
               btype jetton_transfer @final_leg where (.payload_opcode != 0xcbc33949)
               op DedustV2Payout @final_leg
           }
           maybe op Excess @excess }

    # A jetton carrier is a composed block: its `.msg`/`.body` are not reliable
    # (merge_blocks shuffles the event-node order), so every jetton-arm value
    # comes from `.data`. The PayNative parse is harmless on that arm — it is
    # only read under `jetton_in == false`.
    parse ev       as DedustV2SwapEvent

    let l = leg(carrier)
    let jetton_in    = carrier.btype == "jetton_transfer"
    let carrier_cell = (parse? carrier.data.forward_payload as DedustV2PayJetton).payment_payload if jetton_in else carrier.body.payment_payload
    let payload       = parse? carrier_cell as DedustV2SwapPayload
    # PayNative also carries deposits and fund rewards; only a swap payload is ours.
    reject when not jetton_in and payload == null

    require out_leg = first(exit_leg) ?? final_leg
    let lo = leg(out_leg)
    let out_jetton = out_leg.btype == "jetton_transfer"
    # An out leg still carrying a SwapPayload means the route failed mid-hop.
    let out_wrapper = (parse? out_leg.data.forward_payload as DedustV2PayJetton) if out_jetton else null
    reject when (parse? out_wrapper.payment_payload as DedustV2SwapPayload) != null

    let amounts_in  = concat([ev0.body.amount_in],  map(ev.body, "amount_in"))
    let amounts_out = concat([ev0.body.amount_out], map(ev.body, "amount_out"))
    let leg_assets  = concat(map(hop as h => leg(h).asset), [lo.asset])
    let in_assets   = concat([l.asset], leg_assets)
    let hops        = zip(zip(amounts_in, amounts_out), zip(in_assets, leg_assets))

    failed when tx_failed(ev0) or (exit_code(ev0) ?? 0) != 0
    failed when any(ev as e => tx_failed(e) or (exit_code(e) ?? 0) != 0)

    out {
        dex:               "dedust_v2"
        sender:            l.source
        source_asset:      l.asset
        destination_asset: lo.asset
        dex_incoming_transfer: {
            asset:                     l.asset
            amount:                    amount(first(amounts_in))
            source:                    l.source
            source_jetton_wallet:      l.source_wallet
            destination:               l.destination
            destination_jetton_wallet: l.destination_wallet
        }
        dex_outgoing_transfer: {
            asset:                     lo.asset
            amount:                    amount(last(amounts_out))
            source:                    lo.source
            source_jetton_wallet:      lo.source_wallet
            destination:               lo.destination
            destination_jetton_wallet: lo.destination_wallet
        }
        peer_swaps: map(hops as h => {
                        in:  { amount: amount(first(first(h))), asset: first(last(h)) }
                        out: { amount: amount(last(first(h))),  asset: last(last(h)) }
                    }) if len(amounts_in) > 1 else []
        referral_amount:   null
        referral_address:  null
        min_out_amount:    amount(dedust_v2_swap_min_out(payload))
    }
}

# Slippage / deadline failure: the pool refunds instead of swapping, so there is
# no Swap event under the carrier.
#
#   TON-in:    PayNative -> Payout 0x3216ca09 (exit_code != 0, back to the sender)
#   jetton-in: jetton_transfer -> jetton_transfer (same asset, back to the sender)
#
# The refunded route's destination asset is not on the wire at all — it is the
# pool's OTHER asset, read from the pool account's interface data.
matcher dedust_v2_failed_swap {
    produces JettonSwapBlock
    entry    @carrier
    include_excess false

    xor {
        op DedustV2PayNative @carrier as DedustV2PayNative
        btype jetton_transfer @carrier
    }
      -> { or {
               op DedustV2Payout @refund_ton as PayoutMessage
               btype jetton_transfer @refund_jt
           }
           maybe op DedustV2SwapEvent @swapped
           maybe op Excess @excess }


    # A Swap event under the carrier means the swap executed — the success
    # matcher's case (it runs first, so this only fires when IT bailed).
    reject when swapped != null

    let l = leg(carrier)
    let jetton_in    = carrier.btype == "jetton_transfer"
    let carrier_cell = (parse? carrier.data.forward_payload as DedustV2PayJetton).payment_payload if jetton_in else carrier.body.payment_payload
    require payload       = parse? carrier_cell as DedustV2SwapPayload

    # The refund arm must match the carrier arm, and must go back to the
    # carrier's OWN sender: a failed multi-hop refunds the route initiator, so a
    # pool->pool carrier's payout lands elsewhere and is not this swap's refund.
    reject when jetton_in and refund_jt == null
    reject when not jetton_in and refund_ton == null
    reject when not jetton_in and refund_ton.body.exitCode == 0
    reject when not jetton_in and refund_ton.msg.destination != carrier.msg.source
    reject when jetton_in and refund_jt.data.asset != l.asset
    reject when jetton_in and refund_jt.data.receiver != carrier.data.sender

    let sender = l.source
    let pool   = l.destination
    let assets = lookup dedust_pool(pool).assets
    let a0     = first(assets)
    let a1     = last(assets)
    let asset0 = null if a0 == null else (ton_asset() if a0.is_ton else asset(a0.address))
    let asset1 = null if a1 == null else (ton_asset() if a1.is_ton else asset(a1.address))
    # The reference returns the FIRST pool asset that is not the source asset.
    let other  = asset0 if (asset0 != null and asset0 != l.asset) else (asset1 if (asset1 != null and asset1 != l.asset) else null)

    failed when true

    out {
        dex:               "dedust_v2"
        sender:            sender
        source_asset:      l.asset
        destination_asset: other
        dex_incoming_transfer: {
            asset:                     l.asset
            amount:                    carrier.data.amount if jetton_in else amount(carrier.body.amount)
            source:                    sender
            source_jetton_wallet:      l.source_wallet
            destination:               pool
            destination_jetton_wallet: l.destination_wallet
        }
        dex_outgoing_transfer: {
            asset:                     other
            amount:                    amount(0)
            source:                    pool
            source_jetton_wallet:      null
            destination:               sender
            destination_jetton_wallet: null
        }
        peer_swaps:        []
        referral_amount:   null
        referral_address:  null
        min_out_amount:    amount(dedust_v2_swap_min_out(payload))
    }
}
