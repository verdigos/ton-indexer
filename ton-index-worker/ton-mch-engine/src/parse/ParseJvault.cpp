// JVault-family message-body parsers (messages/jvault.py). See parse/PSlice.h
// for shared machinery and MsgParse.cpp's header for the pytoniq-parity
// catalogue. The declarative jvault matchers `parse`:
// JVaultUnstakeJettons (jvault_unstake), JVaultUnstakeRequest
// (jvault_unstake_request) — both op(32) + query_id(u64) + coins — and
// JVaultClaim (jvault_claim): op(32) + query_id(u64) + jettons_to_claim, a
// HashmapE(267) whose keys are MsgAddressStd. jvault_claim became portable once
// its transfer-amount read moved to `withdraw.data.desired_amount.value` (F1+F2)
// — see docs/mch-qol-report.md.
#include "parse/Parsers.h"

#include "parse/PSlice.h"

#include "common/refint.h"
#include "vm/cellslice.h"
#include "vm/dict.h"
#include "vm/cells/CellBuilder.h"

#include <utility>
#include <vector>

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

// claim_rewards#78d9f109 query_id:uint64 jettons_to_claim:(HashmapE 267 Bit).
// messages/jvault.py JVaultClaim + load_address_list: the dict keys ARE the
// MsgAddressStd serialisations (267 bits) and the values are ignored; the
// address list is `list(keys)`. check_for_each visits keys in ascending order;
// the corpus claims carry a single jetton, so list order is not a parity axis.
td::Result<Value> parse_jvault_claim(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64) || !cs.advance(32)) {
    return td::Status::Error("jvault claim: underflow");
  }
  auto query_id = cs.fetch_ulong(64);
  std::vector<Value> jettons;
  // HashmapE 267: hme_empty$0 | hme_root$1 root:^(Hashmap 267 X). Read the Maybe
  // bit and take the root REF ourselves instead of vm::Dictionary{DictAdvance,...},
  // whose label pre-parse can CHECK/abort() on a malformed tail — uncatchable by
  // the MsgParse try/catch and (on Windows Debug) a modal dialog. Constructing the
  // Dictionary from an explicit root cell only ever THROWS VmError on bad nodes,
  // which MsgParse catches -> PARSE_FAIL. Mirrors pytoniq load_address_list:
  // empty dict -> [], keys are 267-bit MsgAddressStd, values ignored, ascending.
  if (!cs.have(1)) {
    return td::Status::Error("jvault claim: dict maybe underflow");
  }
  if (cs.fetch_ulong(1)) {
    if (cs.size_refs() < 1) {
      return td::Status::Error("jvault claim: dict root ref missing");
    }
    td::Status inner = td::Status::OK();
    vm::Dictionary dict{cs.fetch_ref(), 267};
    bool ok = dict.check_for_each(
        [&](td::Ref<vm::CellSlice>, td::ConstBitPtr key, int n) -> bool {
          vm::CellBuilder cb;
          if (!cb.store_bits_bool(key, n)) {
            inner = td::Status::Error("jvault claim: key store failed");
            return false;
          }
          vm::CellSlice keycs = vm::load_cell_slice(cb.finalize());
          auto addr = load_address_py(keycs);
          if (addr.is_error()) {
            inner = addr.move_as_error();
            return false;
          }
          jettons.push_back(addr.move_as_ok());
          return true;
        });
    if (!ok) {
      return inner.is_error() ? std::move(inner)
                              : td::Status::Error("jvault claim: address list parse failed");
    }
  }
  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(query_id)));
  f.emplace_back("jettons_to_claim", Value::make_list(std::move(jettons)));
  return Value::make_obj(std::move(f));
}

}  // namespace mch
