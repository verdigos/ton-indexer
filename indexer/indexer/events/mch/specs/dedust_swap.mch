# DeDust swaps cover routed pool hops and terminal jetton or native payouts.

import "lib/opcodes.mch"

rule peer_chain = or {
                      op DedustSwapPeer {
                            -> { maybe op DedustSwapNotification @peer_notification }
                            -> maybe $peer_chain
                        }
                      op DedustSwapExternal {
                            -> { maybe op DedustSwapNotification @peer_notification }
                            -> maybe $peer_chain
                        }
                      op DedustPayoutFromPool -> or {
                                                                       op DedustPayout
                                                                       btype jetton_transfer
                                                                   }
                  }

matcher dedust_swap {
    produces jetton_swap

    op DedustSwapExternal @ext {
        -> {
            or {
                op DedustPayoutFromPool
                  -> or {
                         btype jetton_transfer
                         op DedustPayout
                     }
                $peer_chain
            }
            maybe op DedustSwapNotification @notification
        }
        <- or {
               btype jetton_transfer @in_transfer
               op DedustSwap @swap_request
           }
    }

    reject when in_transfer != null and in_transfer.data.payload_opcode != 0xe3a0d482
    let is_failed = notification == null or any(peer_notification as n => n == null)
    require legs = dedust_swap_legs(consumed, in_transfer, swap_request, is_failed)

    let notification_legs = map(legs.notifications as n => {
        in:  { amount: amount(n.amount_in),  asset: asset_of(n.asset_in) }
        out: { amount: amount(n.amount_out), asset: asset_of(n.asset_out) }
    })
    let hops = notification_legs if len(notification_legs) > 0 else [legs.fallback_leg]
    let in_leg = first(hops).in
    let out_leg = last(hops).out
    let sender = in_transfer.data.sender if in_transfer != null else swap_request.msg.source
    let source_asset = in_transfer.data.asset if in_transfer != null else ton_asset()

    failed when is_failed
    out {
        dex:               "dedust"
        sender:            sender
        source_asset:      source_asset
        destination_asset: legs.fallback_destination_asset if is_failed else asset_of(last(legs.notifications).asset_out)
        dex_incoming_transfer: {
            asset:                     in_leg.asset
            amount:                    in_leg.amount
            source:                    sender
            source_jetton_wallet:      in_transfer.data.sender_wallet if in_transfer != null else null
            destination:               in_transfer.data.receiver if in_transfer != null else swap_request.msg.destination
            destination_jetton_wallet: in_transfer.data.receiver_wallet if in_transfer != null else null
        }
        dex_outgoing_transfer: {
            asset:                     out_leg.asset
            amount:                    out_leg.amount
            source:                    legs.outgoing_source
            source_jetton_wallet:      legs.outgoing_source_jetton_wallet
            destination:               legs.outgoing_destination ?? sender
            destination_jetton_wallet: legs.outgoing_destination_jetton_wallet
        }
        in:         in_leg
        out:        out_leg
        peer_swaps: hops if len(hops) > 1 else []
    }
}
