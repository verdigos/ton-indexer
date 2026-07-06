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

#include <map>
#include <string>
#include <vector>

namespace mch {

using MsgParserFn = td::Result<Value> (*)(const td::Ref<vm::Cell> &body);

// Message-type registry: IR `parse.types[]` name -> parser. Names are the
// Python class names (Registries.message_types keys).
const std::map<std::string, MsgParserFn> &message_parsers();

// Parse `body` as the named registered type; error when the name is unknown
// or the parse fails (caller implements soft-parse alternatives on top).
td::Result<Value> parse_message_body(const std::string &type_name, const td::Ref<vm::Cell> &body);

// `--parse-dump` mode: for every message in the fixture(s) whose opcode is a
// registered jetton-family opcode, parse the body and print a canonical line
// (see py_msg_dump.py twin). Returns 0 on success.
int run_parse_dump(const std::vector<std::string> &paths);

}  // namespace mch
