tolk 1.0

// Template for a production message-declaration file. Copy to
// abi/<contract>.tolk, fill the struct slot, add the stem to PROD_DECLS in
// tools/gen_prod_abi.mjs, then run `node tools/gen_prod_abi.mjs`.
//
// This file is not compiled because gen_prod_abi.mjs uses an explicit list.
// See ton-abi/ABI-PROTOCOL-CHECKLIST.md for the end-to-end steps.
//
// The remaining contract scaffolding only enables ABI emission and does not
// reach generated parsers. Keep the accepted `tolk 1.0` pragma and name each
// declared message in the union arm.

// ---------------------------------------------------------------------------
// STRUCT SLOT. Add one struct per on-chain message the
// mch spec parses, opcodes + field names/types copied VERBATIM from the
// authoritative contract source. Declare ONLY the messages you parse.
// ---------------------------------------------------------------------------
struct (0x00000001) ExampleMessage {
    queryId: uint64;
    amount: coins;
    someAddress: address;
}

// ---------------------------------------------------------------------------
// Scaffolding below. Name every struct above in the union, nothing else.
// ---------------------------------------------------------------------------
type AllowedMessage = ExampleMessage

struct Storage {
    dummy: uint1
}

contract TemplateContract {
    incomingMessages: AllowedMessage
    storage: Storage
}

fun onInternalMessage(in: InMessage) {
    val msg = lazy AllowedMessage.fromSlice(in.body);
    match (msg) {
        ExampleMessage => {
            assert (msg.amount >= 0) throw 0xFFFF
        }
        else => {
            assert (in.body.isEmpty()) throw 0xFFFF
        }
    }
}
