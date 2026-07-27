// Vesting message parser (messages/vesting.py). See parse/PSlice.h for shared
// machinery and MsgParse.cpp's header for the pytoniq-parity catalogue.
#include "parse/Parsers.h"

#include "parse/PSlice.h"

#include "common/refint.h"
#include "vm/cells/CellBuilder.h"
#include "vm/cellslice.h"

#include <utility>

namespace mch {

namespace {

// pytoniq StateInit.deserialize: split_depth:(Maybe (## 5)) special:(Maybe
// TickTock) code/data/library:(Maybe ^Cell). Only the cursor matters here, but
// every field is walked because Python parses (and can raise on) them all.
td::Status skip_state_init_py(vm::CellSlice &cs) {
  if (!cs.have(1)) return td::Status::Error("state_init: split_depth underflow");
  if (cs.fetch_ulong(1)) {
    if (!cs.have(5)) return td::Status::Error("state_init: split_depth bits underflow");
    cs.advance(5);
  }
  if (!cs.have(1)) return td::Status::Error("state_init: special underflow");
  if (cs.fetch_ulong(1)) {
    if (!cs.have(2)) return td::Status::Error("state_init: tick_tock underflow");
    cs.advance(2);  // TickTock: tick:Bool tock:Bool
  }
  for (int i = 0; i < 3; i++) {  // code, data, library
    if (!cs.have(1)) return td::Status::Error("state_init: maybe-ref underflow");
    if (cs.fetch_ulong(1)) {
      if (cs.size_refs() == 0) return td::Status::Error("state_init: ref missing");
      cs.fetch_ref();
    }
  }
  return td::Status::OK();
}

// pytoniq Slice.to_cell(): Cell(remaining bits, remaining refs).
td::Result<td::Ref<vm::Cell>> slice_to_cell(const vm::CellSlice &cs) {
  try {
    vm::CellBuilder cb;
    cb.store_bits(cs.data_bits(), cs.size());
    for (unsigned i = 0; i < cs.size_refs(); i++) {
      cb.store_ref(cs.prefetch_ref(i));
    }
    return cb.finalize();
  } catch (...) {
    return td::Status::Error("to_cell failed");
  }
}

// MessageAny.deserialize tail, resumed after CommonMsgInfo (which for an
// int_msg_info body is bit-for-bit InternalMsgInfo.deserialize):
//   init:(Maybe (Either StateInit ^StateInit)) body:(Either X ^X)
// Returns the cell Python reads `msg.body.hash` off.
td::Result<td::Ref<vm::Cell>> message_any_body(vm::CellSlice &cs) {
  if (!cs.have(1)) return td::Status::Error("message: init maybe underflow");
  if (cs.fetch_ulong(1)) {
    if (!cs.have(1)) return td::Status::Error("message: init either underflow");
    if (cs.fetch_ulong(1)) {  // right: ^StateInit
      if (cs.size_refs() == 0) return td::Status::Error("message: init ref missing");
      TRY_RESULT(inner, open_ref_cell(cs.fetch_ref()));
      TRY_STATUS(skip_state_init_py(inner));
    } else {  // left: inline
      TRY_STATUS(skip_state_init_py(cs));
    }
  }
  if (!cs.have(1)) return td::Status::Error("message: body either underflow");
  if (cs.fetch_ulong(1)) {  // right: ^X
    if (cs.size_refs() == 0) return td::Status::Error("message: body ref missing");
    return cs.fetch_ref();
  }
  return slice_to_cell(cs);  // left: the rest of the message cell
}

}  // namespace

// VestingSendMessage (messages/vesting.py): the inner message cell is read as a
// full MessageAny (header + init + body), yielding message_body_hash alongside
// destination/value; when only the int_msg_info header parses, Python falls back
// to InternalMsgInfo and leaves message_body_hash None. The header prefix is
// identical in both paths, so the fallback here is just a null hash.
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

  // MessageAny tail; any failure = Python's `except` branch (hash stays null).
  Value body_hash = Value::null();
  auto r_body = message_any_body(ms);
  if (r_body.is_ok()) {
    auto h = r_body.ok()->get_hash();
    td::Slice hs = h.as_slice();
    body_hash = Value::make_bytes(std::string(hs.data(), hs.size()));
  }

  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(query_id)));
  f.emplace_back("send_mode", Value::make_int64(static_cast<std::int64_t>(send_mode)));
  f.emplace_back("message_cell", Value::make_cell(std::move(message_cell)));
  f.emplace_back("message_destination", std::move(dest));
  f.emplace_back("message_value", Value::make_int(std::move(grams)));
  f.emplace_back("message_body_hash", std::move(body_hash));
  return Value::make_obj(std::move(f));
}

}  // namespace mch
