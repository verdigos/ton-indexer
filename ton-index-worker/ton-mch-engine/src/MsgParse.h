// TL-B message-parsing layer: per-family adapters that parse a message body
// cell into a Value map (VType::Obj) whose keys match EXACTLY the attribute
// set the Python reference parser exposes (vars(obj) after __init__), so a
// build program's `CAPTURE.body.field` accesses resolve identically.
//
// Semantics contract (mirrors indexer/events/blocks/messages/jettons.py):
//  - no opcode validation: the first 32 bits are skipped unchecked;
//  - a parse failure anywhere (bit underflow, missing ref, bad address)
//    fails the WHOLE parse — the soft-parse machinery then tries the next
//    alternative / yields null (engine's _soft_parse contract);
//  - addr_none parses to a NULL Value (pytoniq load_address returns None),
//    addr_extern to a "extern:<len>:<value>" Str, addr_std to an Account;
//  - Maybe ^Cell payloads become Bytes (BOC with CRC32C, mirroring
//    pytoniq to_boc(hash_crc32=True)); absent -> null;
//  - integers (uint64 / VarUInteger 16) become Int (RefInt256);
//  - conditionally-set Python attributes (JettonTransfer.sum_type) are
//    conditionally-present keys.
#pragma once

#include "Value.h"

#include "td/utils/Status.h"
#include "vm/cells/Cell.h"
#include "vm/cellslice.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mch {

using MsgParserFn = td::Result<Value> (*)(const td::Ref<vm::Cell> &body);

// Message-type registry: IR `parse.types[]` name -> parser. Names are the
// Python class names (Registries.message_types keys), plus the ABI-bridge rows
// (W12, `<contract>::<decl>` keys). First-source-wins on a key collision.
const std::map<std::string, MsgParserFn> &message_parsers();

// Advisory startup consistency check (W12, design §4): OK unless a key is
// registered by more than one source (hand / schema-generated / ABI bridge), in
// which case an error listing the ambiguous names. Called from startup paths;
// NOT a gate on message_parsers() access (the Meyer's-static map cannot itself
// surface an error, and the no-abort rule forbids failing inside it).
td::Status validate_registries();

// Keys appearing in more than one source, first-wins scan order. Exposed for the
// bridge self-test (W12 --abi-bridge-test) to exercise the dup policy without a
// production collision.
std::vector<std::string> duplicate_parser_keys(
    const std::vector<std::vector<std::pair<std::string, MsgParserFn>>> &sources);

// Parse `body` as the named registered type; error when the name is unknown
// or the parse fails (caller implements soft-parse alternatives on top).
td::Result<Value> parse_message_body(const std::string &type_name, const td::Ref<vm::Cell> &body);

// pytoniq Slice.load_coins() (len nibble 0 -> 0), exported for host predicates
// that parse variable-length headers (HostRegistry, e.g. EVAA).
td::Result<td::RefInt256> pyslice_load_coins(vm::CellSlice &cs);

// blocks/swaps.py _parse_dedust_steps: walk the SwapStep chain from a slice
// positioned at the first step, returning the pool addresses in walk order.
// Exported for the dedust_swap host fn (HostRegistry).
td::Result<std::vector<std::string>> parse_dedust_steps(vm::CellSlice cs);

// TonTransferBlock.comment derivation (basic_blocks.py + common.py
// TonTransferMessage): nullopt == comment None; encrypted comments come back
// base64-encoded; plain comments are utf-8 decoded with backslashreplace and
// U+0000 stripped — exactly the string Python compares. Parse trouble that
// Python's inner try swallows yields nullopt.
std::optional<std::string> ton_transfer_comment(const td::Ref<vm::Cell> &body);

// Decode raw comment bytes with Python's bytes.decode("utf-8",
// errors="backslashreplace").replace("\u0000", "") — the codec the Phase E
// serializer applies to jetton_transfer/nft `comment` payload bytes.
std::string decode_comment_bytes(const std::string &raw);

// (--parse-dump moved to ParseDump.h — §11 item 2 harness isolation.)

}  // namespace mch
