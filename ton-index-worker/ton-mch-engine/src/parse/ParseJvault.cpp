// JVault-family message-body parsers (messages/jvault.py). See parse/PSlice.h
// for shared machinery and MsgParse.cpp's header for the pytoniq-parity
// catalogue. Only the two the declarative jvault matchers `parse`:
// JVaultUnstakeJettons (jvault_unstake) and JVaultUnstakeRequest
// (jvault_unstake_request). Both are op(32) + query_id(u64) + coins. JVaultClaim
// is NOT ported: jvault_claim rejects in the declarative corpus (its
// jvault_transfer_amount host fn reads the legacy `.jetton_transfer_message`
// attribute the IR engine's generic blocks lack) — see docs/mch-qol-report.md.
#include "parse/Parsers.h"

#include "parse/PSlice.h"

#include "common/refint.h"
#include "vm/cellslice.h"

#include <utility>

namespace mch {

namespace {

// op(32) skip + query_id(u64) + jettons_to_unstake(coins). Shared shape of both
// JVaultUnstakeJettons#499a9262 and JVaultUnstakeRequest#0168d4b7 (the Python
// classes parse identically; force_unstake on UnstakeJettons is not read).
td::Result<Value> parse_unstake_common(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64) || !cs.advance(32)) {
    return td::Status::Error("jvault unstake: underflow");
  }
  auto query_id = cs.fetch_ulong(64);
  TRY_RESULT(jettons, load_coins_py(cs));
  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(query_id)));
  f.emplace_back("jettons_to_unstake", Value::make_int(std::move(jettons)));
  return Value::make_obj(std::move(f));
}

}  // namespace

td::Result<Value> parse_jvault_unstake_jettons(const td::Ref<vm::Cell> &body) {
  return parse_unstake_common(body);
}

td::Result<Value> parse_jvault_unstake_request(const td::Ref<vm::Cell> &body) {
  return parse_unstake_common(body);
}

}  // namespace mch
