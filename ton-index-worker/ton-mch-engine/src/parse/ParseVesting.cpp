// Vesting message parser (messages/vesting.py). See parse/PSlice.h for shared
// machinery and MsgParse.cpp's header for the pytoniq-parity catalogue.
#include "parse/Parsers.h"

#include "parse/PSlice.h"

#include "common/refint.h"
#include "vm/cellslice.h"

#include <utility>

namespace mch {

// VestingSendMessage (messages/vesting.py): the inner message cell's
// int_msg_info header is fully deserialized (any failure fails the parse).
// Deviation (documented): a present extra-currency dict ref is consumed but
// not deep-validated (Python's HashMap.parse would validate; vesting inner
// messages never carry extra currencies in practice).
td::Result<Value> parse_vesting_send_message(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64 + 8) || !cs.advance(32)) {
    return td::Status::Error("vesting: header underflow");
  }
  auto query_id = cs.fetch_ulong(64);
  auto send_mode = cs.fetch_ulong(8);
  if (cs.size_refs() == 0) {
    return td::Status::Error("vesting: message_cell ref missing");
  }
  td::Ref<vm::Cell> message_cell = cs.fetch_ref();

  vm::CellSlice ms;
  bool special = false;
  try {
    ms = vm::load_cell_slice_special(message_cell, special);
  } catch (...) {
    return td::Status::Error("vesting: bad message cell");
  }
  // InternalMsgInfo.deserialize: tag bit 0, ihr_disabled/bounce/bounced,
  // src, dest, value (grams + extra dict), ihr_fee, fwd_fee, lt, at.
  if (!ms.have(4)) {
    return td::Status::Error("vesting: msg info underflow");
  }
  if (ms.fetch_ulong(1) != 0) {
    return td::Status::Error("vesting: not int_msg_info");
  }
  ms.advance(3);
  TRY_RESULT(src, load_address_py(ms));
  (void)src;
  TRY_RESULT(dest, load_address_py(ms));
  TRY_RESULT(grams, load_coins_py(ms));
  if (!ms.have(1)) {
    return td::Status::Error("vesting: extra dict underflow");
  }
  if (ms.fetch_ulong(1)) {
    if (ms.size_refs() == 0) {
      return td::Status::Error("vesting: extra dict ref missing");
    }
    ms.fetch_ref();
  }
  TRY_RESULT(ihr_fee, load_coins_py(ms));
  (void)ihr_fee;
  TRY_RESULT(fwd_fee, load_coins_py(ms));
  (void)fwd_fee;
  if (!ms.have(64 + 32)) {
    return td::Status::Error("vesting: lt/at underflow");
  }
  ms.advance(64 + 32);

  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(query_id)));
  f.emplace_back("send_mode", Value::make_int64(static_cast<std::int64_t>(send_mode)));
  f.emplace_back("message_cell", Value::make_cell(std::move(message_cell)));
  f.emplace_back("message_destination", std::move(dest));
  f.emplace_back("message_value", Value::make_int(std::move(grams)));
  return Value::make_obj(std::move(f));
}

}  // namespace mch
