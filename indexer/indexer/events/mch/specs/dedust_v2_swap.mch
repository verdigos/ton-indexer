# DeDust V2 swap matching covers successful routes through one or more pools and failed routes that refund the input.

import "lib/common.mch"
import "lib/leg.mch"
import "lib/opcodes.mch"

opcode DedustV2SwapEvent = 0x78e79ba4
opcode DedustV2Payout    = 0x3216ca09
rule dv2_hop = or {
                   op DedustV2PayNative @hop
                     -> { op DedustV2SwapEvent @ev -> maybe $dv2_hop }
                   btype jetton_transfer @hop where (.payload_opcode == 0xcbc33949)
                     -> { op DedustV2SwapEvent @ev -> maybe $dv2_hop }
                   btype jetton_transfer @exit_leg where (.payload_opcode != 0xcbc33949)
                   op DedustV2Payout @exit_leg
               }

matcher dedust_v2_swap {
    produces jetton_swap
    include_excess false

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
           maybe op Excess }

    parse ev       as DedustV2SwapEvent

    let l = leg(carrier)
    let jetton_in    = carrier.btype == "jetton_transfer"
    let carrier_cell = (parse? carrier.data.forward_payload as DedustV2PayJetton).payment_payload if jetton_in else carrier.body.payment_payload
    let payload       = parse? carrier_cell as DedustV2SwapPayload
    # PayNative also carries deposits and rewards, so a missing swap payload excludes it.
    reject when not jetton_in and payload == null

    require out_leg = first(exit_leg) ?? final_leg
    let lo = leg(out_leg)

    let amounts_in  = concat([ev0.body.amount_in],  map(ev.body, "amount_in"))
    let amounts_out = concat([ev0.body.amount_out], map(ev.body, "amount_out"))
    let leg_assets  = concat(map(hop as h => leg(h).asset), [lo.asset])
    let in_assets   = concat([l.asset], leg_assets)
    let hops        = zip(zip(amounts_in, amounts_out), zip(in_assets, leg_assets))

    failed when any(concat([ev0], ev) as e => tx_failed(e) or (exit_code(e) ?? 0) != 0)

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

matcher dedust_v2_failed_swap {
    produces jetton_swap
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
           maybe op Excess }

    reject when swapped != null

    let l = leg(carrier)
    let jetton_in    = carrier.btype == "jetton_transfer"
    let carrier_cell = (parse? carrier.data.forward_payload as DedustV2PayJetton).payment_payload if jetton_in else carrier.body.payment_payload
    require payload       = parse? carrier_cell as DedustV2SwapPayload

    # A failed multihop carrier can refund the route initiator elsewhere, so the refund must return to this carrier's sender.
    reject when jetton_in and refund_jt == null
    reject when not jetton_in and refund_ton == null
    reject when not jetton_in and refund_ton.body.exitCode == 0
    reject when not jetton_in and refund_ton.msg.destination != carrier.msg.source
    reject when jetton_in and refund_jt.data.asset != l.asset
    reject when jetton_in and refund_jt.data.receiver != carrier.data.sender

    let sender = l.source
    let pool   = l.destination
    # Pool lookup miss leaves the destination asset null while the failed swap row remains.
    let assets = lookup dedust_pool(pool).assets
    let a0     = first(assets)
    let a1     = last(assets)
    let asset0 = null if a0 == null else (ton_asset() if a0.is_ton else asset(a0.address))
    let asset1 = null if a1 == null else (ton_asset() if a1.is_ton else asset(a1.address))
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
