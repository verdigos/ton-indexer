#include "host/HostImpls.h"

#include "host/HostCommon.h"

#include "BlockTree.h"
#include "parse/PSlice.h"
#include "parse/Parsers.h"
#include "btypes_gen.h"

#include "common/refint.h"
#include "vm/cellslice.h"

#include <utility>

namespace mch {

namespace {

// Skip the EVAA user header (version coins + maybe upgrade-info ref + 2 exec
// bits), leaving the cursor on the 32-bit opcode.
bool skip_user_header(vm::CellSlice &cs) {
  if (load_coins_py(cs).is_error()) {  // user_version
    return false;
  }
  if (!cs.have(1)) {  // load_maybe_ref: the Maybe bit
    return false;
  }
  if (cs.fetch_ulong(1)) {
    if (cs.size_refs() == 0) {
      return false;
    }
    cs.fetch_ref();  // upgrade_info
  }
  if (!cs.have(2)) {  // upgrade_exec
    return false;
  }
  cs.advance(2);
  return true;
}

}  // namespace

td::Result<Value> parse_evaa_supply_user_hdr(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!skip_user_header(cs) || !cs.have(32 + 64 + 256)) {
    return td::Status::Error("supply_user underflow");
  }
  cs.advance(32 + 64);  // opcode + query_id
  auto asset_id = cs.fetch_int256(256, false);
  if (asset_id.is_null()) {
    return td::Status::Error("supply_user asset_id");
  }
  return Value::make_obj({{"asset_id", Value::make_int(std::move(asset_id))}});
}

td::Result<Value> parse_evaa_user_op(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!skip_user_header(cs) || !cs.have(32)) {
    return td::Status::Error("user_op underflow");
  }
  return Value::make_obj({{"op", Value::make_int(refint_u64(cs.fetch_ulong(32)))}});
}

}  // namespace mch
