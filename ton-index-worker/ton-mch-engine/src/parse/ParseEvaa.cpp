// EVAA withdraw-family message parsers (messages/evaa.py). See parse/PSlice.h
// for shared machinery and MsgParse.cpp's header for the pytoniq-parity
// catalogue.
#include "parse/Parsers.h"

#include "parse/PSlice.h"

#include "common/refint.h"
#include "vm/cellslice.h"

#include <utility>

namespace mch {

// withdraw_master#00000002: query_id u64, asset_id u256, amount u64, recipient.
td::Result<Value> parse_evaa_withdraw_master(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64 + 256 + 64) || !cs.advance(32)) {
    return td::Status::Error("evaa master: underflow");
  }
  auto query_id = cs.fetch_ulong(64);
  auto asset_id = cs.fetch_int256(256, false);
  if (asset_id.is_null()) {
    return td::Status::Error("evaa master: asset_id fetch failed");
  }
  auto amount = cs.fetch_ulong(64);
  TRY_RESULT(recipient, load_address_py(cs));
  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(query_id)));
  f.emplace_back("asset_id", Value::make_int(std::move(asset_id)));
  f.emplace_back("amount", Value::make_int(refint_u64(amount)));
  f.emplace_back("recipient_address", std::move(recipient));
  return Value::make_obj(std::move(f));
}

// withdraw_collateralized#00000211: query_id u64, owner addr, asset_id u256,
// withdraw_amount_current u64 (rest skipped like the Python parser).
td::Result<Value> parse_evaa_withdraw_collateralized(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64) || !cs.advance(32)) {
    return td::Status::Error("evaa coll: underflow");
  }
  auto query_id = cs.fetch_ulong(64);
  TRY_RESULT(owner, load_address_py(cs));
  if (!cs.have(256 + 64)) {
    return td::Status::Error("evaa coll: underflow");
  }
  auto asset_id = cs.fetch_int256(256, false);
  if (asset_id.is_null()) {
    return td::Status::Error("evaa coll: asset_id fetch failed");
  }
  auto amount = cs.fetch_ulong(64);
  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(query_id)));
  f.emplace_back("owner_address", std::move(owner));
  f.emplace_back("asset_id", Value::make_int(std::move(asset_id)));
  f.emplace_back("withdraw_amount_current", Value::make_int(refint_u64(amount)));
  return Value::make_obj(std::move(f));
}

// EvaaWithdrawFailExcess: opcode-tagged reason; an unknown opcode raises in
// Python (ValueError) -> soft-parse failure.
td::Result<Value> parse_evaa_withdraw_fail_excess(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32)) {
    return td::Status::Error("evaa fail excess: underflow");
  }
  auto op = static_cast<td::uint32>(cs.fetch_ulong(32));
  const char *reason;
  if (op == 0x21e6) {
    reason = "withdraw_locked_excess";
  } else if (op == 0x21e7) {
    reason = "withdraw_not_collateralized_excess";
  } else if (op == 0x21e8) {
    reason = "withdraw_missing_prices_excess";
  } else if (op == 0x21ec) {
    reason = "withdraw_execution_crashed";
  } else {
    return td::Status::Error("evaa fail excess: unknown opcode");
  }
  Value::Fields f;
  f.emplace_back("opcode", Value::make_int64(static_cast<std::int64_t>(op)));
  f.emplace_back("reason", Value::make_str(reason));
  return Value::make_obj(std::move(f));
}

}  // namespace mch
