// Ston.fi v2 message parsers (messages/liquidity.py). See parse/PSlice.h for
// shared machinery and MsgParse.cpp's header for the pytoniq-parity catalogue.
#include "parse/Parsers.h"

#include "parse/PSlice.h"

#include "common/refint.h"
#include "vm/cellslice.h"

#include <utility>

namespace mch {

// StonfiV2ProvideLiquidity (messages/liquidity.py): opcode(32) skipped, then
// query_id:uint64, from_user:MsgAddress, amount1:(VarUInteger 16),
// amount2:(VarUInteger 16). `from_user` is a raw pytoniq load_address() result
// (Account Value, addr_none -> null); the two amounts are plain coins ints.
td::Result<Value> parse_stonfi_v2_provide_liquidity(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64) || !cs.advance(32)) {
    return td::Status::Error("stonfi_v2_provide: header underflow");
  }
  auto query_id = cs.fetch_ulong(64);
  TRY_RESULT(from_user, load_address_py(cs));
  TRY_RESULT(amount1, load_coins_py(cs));
  TRY_RESULT(amount2, load_coins_py(cs));
  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(query_id)));
  f.emplace_back("from_user", std::move(from_user));
  f.emplace_back("amount1", Value::make_int(std::move(amount1)));
  f.emplace_back("amount2", Value::make_int(std::move(amount2)));
  return Value::make_obj(std::move(f));
}

}  // namespace mch
