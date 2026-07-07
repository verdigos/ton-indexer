// pTON message parser (messages/swaps.py). See parse/PSlice.h for shared
// machinery and MsgParse.cpp's header for the pytoniq-parity catalogue.
#include "parse/Parsers.h"

#include "parse/PSlice.h"

#include "common/refint.h"
#include "vm/cellslice.h"
#include "vm/cells/CellBuilder.h"

#include <utility>

namespace mch {

// PTonTransfer (messages/swaps.py): forward_payload = maybe-ref Cell, else the
// slice REMAINDER as a cell when refs remain (pytoniq body.to_cell(): remaining
// bits + refs FROM the current ref cursor — NOT the copy() reset).
td::Result<Value> parse_pton_transfer(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64) || !cs.advance(32)) {
    return td::Status::Error("pton: header underflow");
  }
  auto query_id = cs.fetch_ulong(64);
  TRY_RESULT(ton_amount, load_coins_py(cs));
  TRY_RESULT(refund_address, load_address_py(cs));
  if (!cs.have(1)) {
    return td::Status::Error("pton: forward_payload bit missing");
  }
  Value forward_payload = Value::null();
  if (cs.fetch_ulong(1)) {
    if (cs.size_refs() == 0) {
      return td::Status::Error("pton: forward_payload ref missing");
    }
    forward_payload = Value::make_cell(cs.fetch_ref());
  } else if (cs.size_refs() > 0) {
    try {
      vm::CellBuilder cb;
      cb.store_bits(cs.data_bits(), cs.size());
      for (unsigned i = 0; i < cs.size_refs(); i++) {
        cb.store_ref(cs.prefetch_ref(i));
      }
      forward_payload = Value::make_cell(cb.finalize());
    } catch (...) {
      return td::Status::Error("pton: to_cell failed");
    }
  }
  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(query_id)));
  f.emplace_back("ton_amount", Value::make_int(std::move(ton_amount)));
  f.emplace_back("refund_address", std::move(refund_address));
  f.emplace_back("forward_payload", std::move(forward_payload));
  return Value::make_obj(std::move(f));
}

}  // namespace mch
