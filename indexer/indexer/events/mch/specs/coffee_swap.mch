# Coffee swaps require a success event at each pool hop and end in a payout.
# Captured events supply hop amounts and assets in route order; the incoming
# transfer and terminal payout supply the transfer legs.

import "lib/leg.mch"

opcode CoffeeSwapNative          = 0xc0ffee00
opcode CoffeeSwapInternal        = 0xc0ffee20
opcode CoffeeSwapSuccessfulEvent = 0xc0ffee30
opcode CoffeePayoutInternal      = 0xc0ffee21
opcode CoffeePayout              = 0xc0ffee32
opcode CoffeeNotification        = 0xc0ffee36

# One pool hop per unrolling: a swap request with its success event; exit =
# the payout chain to the user.
rule swap_chain = xor {
                      op CoffeeSwapInternal @peer -> op CoffeeSwapSuccessfulEvent @peer_event as CoffeeSwapEvent -> maybe $swap_chain
                      op CoffeePayoutInternal @exit_payout as CoffeePayoutInternal -> or {
                                                     btype jetton_transfer @exit_out
                                                     op CoffeePayout @exit_out
                                                     op CoffeeNotification @exit_out
                                                 }
                  }

matcher coffee_swap {
    produces JettonSwapBlock
    entry    @swap

    op CoffeeSwapInternal @swap {
        -> {
            op CoffeeSwapSuccessfulEvent @event as CoffeeSwapEvent
            ; or {
                  $swap_chain
                  op CoffeePayoutInternal @payout as CoffeePayoutInternal
                  -> xor {
                         btype jetton_transfer @out_transfer
                         op CoffeePayout @out_transfer
                         op CoffeeNotification @out_transfer
                     }
              }
        }
        <- xor {
               op CoffeeSwapNative @in_transfer as CoffeeSwapNative
               btype jetton_transfer @in_transfer
           }
    }

    require po = first(exit_payout) ?? payout
    require ol = first(exit_out) ?? out_transfer
    let li = leg(in_transfer)
    let ton_payout = ol.btype != "jetton_transfer" and ol.data.opcode == 0xc0ffee32
    reject when event.body == null or any(peer_event.body as e => e == null)
    reject when li.is_ton and in_transfer.body == null
    reject when ton_payout and po.body == null

    let lo = leg(ol) if ol.btype == "jetton_transfer" else (
        ton_leg(amount(po.body.amount), po.msg.source, account(po.body.recipient) ?? addr_none()) if ton_payout else ton_leg(amount(ol.msg.value), ol.msg.source, ol.msg.destination))
    let bodies = concat([event.body], peer_event.body)
    let peer_assets = map(peer_event.body as e => asset_of(e.asset))
    let hops = zip(zip(map(bodies, "amount_in"), map(bodies, "amount_out")),
                   zip(concat([asset_of(event.body.asset)], peer_assets), concat(peer_assets, [lo.asset])))

    out {
        dex:               "coffee"
        sender:            li.source
        source_asset:      li.asset
        destination_asset: lo.asset
        dex_incoming_transfer: {
            asset:                     li.asset
            amount:                    amount(in_transfer.body.amount) if li.is_ton else li.amount
            source:                    li.source
            source_jetton_wallet:      li.source_wallet
            destination:               li.destination
            destination_jetton_wallet: li.destination_wallet
        }
        dex_outgoing_transfer: {
            asset:                     lo.asset
            amount:                    lo.amount
            source:                    lo.source
            source_jetton_wallet:      lo.source_wallet
            destination:               lo.destination
            destination_jetton_wallet: lo.destination_wallet
        }
        peer_swaps: map(hops as h => {
                        in:  { asset: first(last(h)), amount: amount(first(first(h))) }
                        out: { asset: last(last(h)),  amount: amount(last(first(h))) }
                    }) if len(peer_event) > 0 else []
        referral_amount:  null
        referral_address: null
    }
}
