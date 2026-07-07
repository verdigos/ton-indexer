// Shared low-level helpers for the host binding TUs (see host/HostCommon.h).
#include "host/HostCommon.h"

#include "BlockTree.h"
#include "TraceLoader.h"

#include "td/utils/base64.h"
#include "vm/boc.h"

namespace mch {

td::Result<td::Ref<vm::Cell>> block_body(const Block *b) {
  if (b->event_nodes.empty() || b->event_nodes.front()->msg == nullptr ||
      !b->event_nodes.front()->msg->content) {
    return td::Status::Error("no body");
  }
  TRY_RESULT(raw, td::base64_decode(td::Slice(b->event_nodes.front()->msg->content->body)));
  return vm::std_boc_deserialize(raw);
}

const Block *as_block(const Value &v) { return v.t == VType::Block ? v.block : nullptr; }

bool is_call_op(const Block *b, std::uint32_t op) {
  return b != nullptr && b->btype == "call_contract" && b->opcode && *b->opcode == op;
}

// AccountId(envelope-address-str).as_str(): canonical "wc:HEX" upper; a null or
// unparseable address -> addr_none (AccountId(None)).
Value account_from_opt(const std::optional<std::string> &s) {
  if (!s) {
    return Value::make_account_none();
  }
  auto norm = normalize_raw_address(*s);
  return norm ? Value::make_account_raw(*norm) : Value::make_account_none();
}

const Message *block_msg(const Block *b) {
  return b->event_nodes.empty() ? nullptr : b->event_nodes.front()->msg;
}

// Amount(x): a numeric-carrying Value (Int/Amount) -> Amount with that value.
Value to_amount(const Value &v) {
  return (v.t == VType::Int || v.t == VType::Amount) ? Value::make_amount(v.num)
                                                     : Value::make_amount_none();
}

// Copy a field out of a Block's data Dict (Null when absent).
Value data_field(const Block *b, const char *name) {
  const Value *f = b->data.field(name);
  return f != nullptr ? *f : Value::null();
}

}  // namespace mch
