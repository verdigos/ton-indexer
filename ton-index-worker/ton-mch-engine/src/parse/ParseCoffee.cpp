// Coffee message parser (messages/coffee.py). See parse/PSlice.h for shared
// machinery and MsgParse.cpp's header for the pytoniq-parity catalogue.
#include "parse/Parsers.h"

#include "parse/PSlice.h"

#include "common/refint.h"
#include "vm/cellslice.h"

#include <string>
#include <utility>

namespace mch {

// CoffeeCreateVault (messages/coffee.py + load_coffee_asset): native/jetton
// asset tags; extra$10 and the invalid tag raise in Python -> parse fail.
td::Result<Value> parse_coffee_create_vault(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64) || !cs.advance(32)) {
    return td::Status::Error("coffee vault: header underflow");
  }
  auto query_id = cs.fetch_ulong(64);
  if (!cs.have(2)) {
    return td::Status::Error("coffee vault: asset tag underflow");
  }
  auto tag = cs.fetch_ulong(2);
  Value asset;
  if (tag == 0) {
    asset = Value::make_asset_ton();
  } else if (tag == 1) {
    if (!cs.have(8)) {
      return td::Status::Error("coffee vault: asset chain underflow");
    }
    int chain = static_cast<int>(cs.fetch_ulong(8));
    if (!cs.have(256)) {
      return td::Status::Error("coffee vault: asset hash underflow");
    }
    unsigned char buf[32];
    if (!cs.fetch_bytes(buf, 32)) {
      return td::Status::Error("coffee vault: asset hash fetch failed");
    }
    asset = Value::make_asset_jetton(std::to_string(chain) + ":" + hex_upper(buf, 32));
  } else {
    // extra$10 -> NotImplementedError; anything else -> ValueError.
    return td::Status::Error("coffee vault: unsupported asset tag");
  }
  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(query_id)));
  f.emplace_back("asset", std::move(asset));
  return Value::make_obj(std::move(f));
}

}  // namespace mch
