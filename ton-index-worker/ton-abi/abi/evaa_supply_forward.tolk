tolk 1.0

// Faithful wire declarations for schema-migrated evaa_supply_forward messages.

@overflow1023_policy("suppress")
struct (0x00000001) EvaaSupplyJettonForward {
    _include_user_code: bits2;
    recipient_address: any_address;
}

type AllowedMessage = EvaaSupplyJettonForward

struct Storage {
    dummy: uint1
}

contract EvaaSupplyForwardMessages {
    incomingMessages: AllowedMessage
    storage: Storage
}

fun onInternalMessage(in: InMessage) {
    val msg = lazy AllowedMessage.fromSlice(in.body);
    match (msg) {
        EvaaSupplyJettonForward => {
            assert (in.body.isEmpty()) throw 0xFFFF
        }
        else => {
            assert (in.body.isEmpty()) throw 0xFFFF
        }
    }
}
