# The transfer fields carried by a jetton or native leg.
export fn leg(x) = null if x == null else {
    is_ton:             x.btype != "jetton_transfer"
    asset:              x.data.asset           if x.btype == "jetton_transfer" else ton_asset()
    amount:             x.data.amount          if x.btype == "jetton_transfer" else (amount(x.msg.value) if x.btype == "ton_transfer" else null)
    source:             x.data.sender          if x.btype == "jetton_transfer" else account(x.msg.source)
    source_wallet:      x.data.sender_wallet   if x.btype == "jetton_transfer" else null
    destination:        x.data.receiver        if x.btype == "jetton_transfer" else account(x.msg.destination)
    destination_wallet: x.data.receiver_wallet if x.btype == "jetton_transfer" else null
    payload:            x.data.forward_payload if x.btype == "jetton_transfer" else null
}

# Builds a TON transfer leg from its amount and accounts.
export fn ton_leg(amt, src, dst) = {
    is_ton:             true
    asset:              ton_asset()
    amount:             amt
    source:             src
    source_wallet:      null
    destination:        dst
    destination_wallet: null
    payload:            null
}
