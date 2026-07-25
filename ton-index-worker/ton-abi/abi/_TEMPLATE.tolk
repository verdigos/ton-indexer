tolk 1.0

// TEMPLATE for a PRODUCTION message-declaration file (plan §0.11). Copy to
// abi/<contract>.tolk, fill the struct slot, add the stem to PROD_DECLS in
// tools/gen_prod_abi.mjs, then run `node tools/gen_prod_abi.mjs`.
//
// This file is NOT compiled: gen_prod_abi.mjs drives an explicit PROD_DECLS
// list, and the ton-abi --check glob covers abi/*.abi.json (which a template
// never has). Full end-to-end steps: ton-abi/ABI-PROTOCOL-CHECKLIST.md.
//
// Everything below the struct slot is COMPILER APPEASEMENT ONLY: tolk needs a
// contract with a storage type and an entrypoint before it will emit an ABI,
// but none of it reaches the generated parsers. Keep it as-is; only the union
// arm list has to name your structs. The `tolk 1.0` pragma matches what the
// pinned @ton/tolk-js accepts here (the T12.2 design text said 1.4.1; the
// as-built file uses 1.0 and regenerates byte-identically — do not "fix" it
// without regenerating the committed .abi.json + generated pair).

// ---------------------------------------------------------------------------
// STRUCT SLOT — the only part you edit. One struct per on-chain message the
// mch spec parses, opcodes + field names/types copied VERBATIM from the
// authoritative contract source. Declare ONLY the messages you parse.
// ---------------------------------------------------------------------------
struct (0x00000001) ExampleMessage {
    queryId: uint64;
    amount: coins;
    someAddress: address;
}

// ---------------------------------------------------------------------------
// Scaffolding below — name every struct above in the union, nothing else.
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
