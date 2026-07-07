// Shared low-level helpers for the per-protocol host binding TUs (src/host/*).
// Extracted verbatim from HostRegistry.cpp so the split predicate/fn files can
// share them; see HostRegistry.h for the public registry surface.
#pragma once

#include "Value.h"

#include "td/utils/Status.h"
#include "vm/cells/Cell.h"

#include <cstdint>
#include <optional>
#include <string>

namespace mch {

struct Block;     // BlockTree.h
struct Message;   // TraceLoader.h

td::Result<td::Ref<vm::Cell>> block_body(const Block *b);

const Block *as_block(const Value &v);

bool is_call_op(const Block *b, std::uint32_t op);

// AccountId(envelope-address-str).as_str(): canonical "wc:HEX" upper; a null or
// unparseable address -> addr_none (AccountId(None)).
Value account_from_opt(const std::optional<std::string> &s);

const Message *block_msg(const Block *b);

// Amount(x): a numeric-carrying Value (Int/Amount) -> Amount with that value.
Value to_amount(const Value &v);

// Copy a field out of a Block's data Dict (Null when absent).
Value data_field(const Block *b, const char *name);

}  // namespace mch
