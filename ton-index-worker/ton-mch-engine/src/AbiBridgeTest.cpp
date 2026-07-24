// W12 bridge self-test. See AbiBridgeTest.h / W12-BRIDGE-DESIGN.md §7.
#include "AbiBridgeTest.h"

#include "AbiBridge.h"
#include "MsgParse.h"
#include "Value.h"

#include "AbiGenSupport.h"  // ton_abi::gen::address_from_string
#include "AbiValue.h"
#include "AbiLeavesAddress.h"

#include "tolk_counter_gen.h"
#include "jetton_wallet_contract_gen.h"
#include "lots_of_wrappers_gen.h"

#include "common/refint.h"
#include "vm/cells/CellBuilder.h"
#include "vm/cellslice.h"

#include <cstdio>
#include <string>
#include <variant>
#include <vector>

namespace mch {

namespace {

namespace tc = ton_abi::gen::tolk_counter;
namespace jw = ton_abi::gen::jetton_wallet_contract;
namespace lw = ton_abi::gen::lots_of_wrappers;
using ton_abi::AbiAddress;
using ton_abi::AbiAddressKind;
using ton_abi::AbiValue;

int g_fail = 0;

void check(const std::string &name, bool ok) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
  if (!ok) {
    g_fail++;
  }
}

// ---- Value inspectors -----------------------------------------------------
bool is_int(const Value *v, long long want) {
  return v && v->t == VType::Int && v->num.not_null() && v->num->to_dec_string() == std::to_string(want);
}
bool is_bool(const Value *v, bool want) {
  return v && v->t == VType::Bool && v->boolean == want;
}
bool is_account(const Value *v) {  // non-none Account
  return v && v->t == VType::Account && !v->addr_none && !v->str.empty();
}
const Value *fld(const Value &v, const char *name) { return v.field(name); }

// ---- cell-building helpers ------------------------------------------------
td::Ref<vm::Cell> empty_cell() { return vm::CellBuilder().finalize(); }
td::Ref<vm::CellSlice> empty_slice() { return vm::load_cell_slice_ref(empty_cell()); }
AbiAddress test_addr() { return ton_abi::gen::address_from_string("0:" + std::string(64, 'a')); }

template <class T>
td::Result<Value> round_trip(const T &s) {
  vm::CellBuilder cb;
  auto st = s.store(cb);
  if (st.is_error()) {
    return std::move(st);
  }
  return abi_parse_body<T>(cb.finalize());
}

// ---- full-trampoline-path fixtures ----------------------------------------

// Simple prefixed struct: prefix check + uint64 -> Int + Struct -> Obj("$").
void test_increase_counter() {
  auto r = round_trip(tc::IncreaseCounter::create({/*queryId*/ 7ULL, /*increaseBy*/ 42ULL}));
  bool ok = r.is_ok();
  check("counter/parses", ok);
  if (!ok) return;
  const Value &v = r.ok();
  check("counter/is_obj", v.t == VType::Obj);
  check("counter/discriminator", fld(v, "$") && fld(v, "$")->str == "IncreaseCounter");
  check("counter/queryId", is_int(fld(v, "queryId"), 7));
  check("counter/increaseBy", is_int(fld(v, "increaseBy"), 42));
}

// Jetton fixture: RefInt256 coins -> Int + AbiAddress Std -> Account + Cell.
void test_jetton_wallet_data() {
  jw::JettonWalletDataReply::CreateArgs args;
  args.jettonBalance = td::make_refint(123456);
  args.ownerAddress = test_addr();
  args.minterAddress = test_addr();
  args.jettonWalletCode = empty_cell();
  auto r = round_trip(jw::JettonWalletDataReply::create(std::move(args)));
  bool ok = r.is_ok();
  check("jetton/parses", ok);
  if (!ok) return;
  const Value &v = r.ok();
  check("jetton/discriminator", fld(v, "$") && fld(v, "$")->str == "JettonWalletDataReply");
  check("jetton/balance", is_int(fld(v, "jettonBalance"), 123456));
  check("jetton/owner_account", is_account(fld(v, "ownerAddress")));
  check("jetton/minter_account", is_account(fld(v, "minterAddress")));
  check("jetton/code_cell", fld(v, "jettonWalletCode") && fld(v, "jettonWalletCode")->t == VType::Cell);
}

// has_value_field union: primitive variant -> Union kind -> Obj{"$","value"}.
void test_hasvalue_union() {
  lw::AbiUnion_0 u{td::int64(5)};  // int8 variant
  auto r = round_trip(lw::IntAndEitherInt8Or256::create({td::int64(9), u}));
  bool ok = r.is_ok();
  check("union_hasvalue/parses", ok);
  if (!ok) return;
  const Value &v = r.ok();
  check("union_hasvalue/op", is_int(fld(v, "op"), 9));
  const Value *un = fld(v, "i8or256");
  check("union_hasvalue/is_obj", un && un->t == VType::Obj);
  check("union_hasvalue/has_label", un && fld(*un, "$") && fld(*un, "$")->t == VType::Str);
  check("union_hasvalue/value_int", un && is_int(fld(*un, "value"), 5));
}

// Struct-labeled union: variant IS its inner struct (carries own "$"), NOT
// wrapped in {"$","value"} — the key design distinction. Also covers Address +
// Bool + Bits inside the inner struct + an 8-bit prefix.
void test_structlabeled_union() {
  lw::BodyPayload body{lw::BodyPayload1::create({/*should_forward*/ true, /*n_times*/ 3, empty_slice()})};
  auto r = round_trip(lw::SayHiAndGoodbye::create({test_addr(), std::move(body)}));
  bool ok = r.is_ok();
  check("union_structlabeled/parses", ok);
  if (!ok) return;
  const Value &v = r.ok();
  check("union_structlabeled/dest_account", is_account(fld(v, "dest_addr")));
  const Value *b = fld(v, "body");
  check("union_structlabeled/body_is_obj", b && b->t == VType::Obj);
  // Inner struct carries its OWN "$" = variant struct name (NOT a "value" wrapper).
  check("union_structlabeled/inner_discriminator", b && fld(*b, "$") && fld(*b, "$")->str == "BodyPayload1");
  check("union_structlabeled/no_value_wrapper", b && fld(*b, "value") == nullptr);
  check("union_structlabeled/inner_bool", b && is_bool(fld(*b, "should_forward"), true));
  check("union_structlabeled/inner_int", b && is_int(fld(*b, "n_times"), 3));
}

// ---- direct AbiValue -> Value adapter units (kinds no fixture reaches) -----
void test_adapter_direct() {
  {
    Value v = abi_value_to_mch(AbiValue::make_bool(true));
    check("adapter/bool", is_bool(&v, true));
  }
  check("adapter/null", abi_value_to_mch(AbiValue::make_null()).is_null());

  {
    Value v = abi_value_to_mch(AbiValue::make_void());
    check("adapter/void", v.t == VType::Obj && fld(v, "$") && fld(v, "$")->str == "void");
  }
  {
    AbiAddress none;  // kind == None by default
    Value v = abi_value_to_mch(AbiValue::make_address(none));
    check("adapter/addr_none", v.t == VType::Account && v.addr_none);
  }
  {
    AbiAddress ext;
    ext.kind = AbiAddressKind::Extern;
    ext.ext_bits = 10;
    ext.ext_value = empty_slice();
    Value v = abi_value_to_mch(AbiValue::make_address(ext));
    check("adapter/extern", v.t == VType::Obj && is_int(fld(v, "bits"), 10) &&
                                fld(v, "value") && fld(v, "value")->t == VType::Cell);
  }
  {
    Value v = abi_value_to_mch(AbiValue::make_bits(empty_slice()));
    check("adapter/bits_is_cell", v.t == VType::Cell);
  }
  {
    Value v = abi_value_to_mch(AbiValue::make_cell_of(AbiValue::make_int(td::make_refint(5))));
    check("adapter/cellof", v.t == VType::Obj && is_int(fld(v, "ref"), 5));
  }
  {
    std::vector<AbiValue> items;
    items.push_back(AbiValue::make_int(td::make_refint(1)));
    items.push_back(AbiValue::make_int(td::make_refint(2)));
    Value v = abi_value_to_mch(AbiValue::make_list(std::move(items)));
    check("adapter/list", v.t == VType::List && v.items && v.items->size() == 2 &&
                              is_int(&(*v.items)[0], 1) && is_int(&(*v.items)[1], 2));
  }
  {
    std::vector<std::pair<AbiValue, AbiValue>> entries;
    entries.emplace_back(AbiValue::make_int(td::make_refint(1)), AbiValue::make_int(td::make_refint(2)));
    Value v = abi_value_to_mch(AbiValue::make_map(std::move(entries)));
    bool ok = v.t == VType::List && v.items && v.items->size() == 1;
    ok = ok && (*v.items)[0].t == VType::List && (*v.items)[0].items && (*v.items)[0].items->size() == 2;
    ok = ok && is_int(&(*(*v.items)[0].items)[0], 1) && is_int(&(*(*v.items)[0].items)[1], 2);
    check("adapter/map_is_pairlist", ok);
  }
  {
    Value v = abi_value_to_mch(AbiValue::make_cell(empty_cell()));
    check("adapter/cell", v.t == VType::Cell);
  }
}

// ---- registry / trampoline-guard policy -----------------------------------
void test_registry_policy() {
  // Duplicate key across sources -> reported (design §4 seam).
  std::vector<std::vector<std::pair<std::string, MsgParserFn>>> sources = {
      {{"A", nullptr}, {"B", nullptr}},
      {{"B", nullptr}, {"C", nullptr}},  // B collides
  };
  auto dups = duplicate_parser_keys(sources);
  check("registry/dup_detected", dups.size() == 1 && dups[0] == "B");

  // Production registry has no collision.
  check("registry/prod_clean", validate_registries().is_ok());

  // Unknown name fails closed.
  auto r = parse_message_body("__NoSuchType__", empty_cell());
  check("registry/unknown_name", r.is_error());

  // Abort-safe open guard: null body -> clean error result (never an abort);
  // the exotic-cell branch shares this code path (special flag).
  auto rn = abi_parse_body<tc::IncreaseCounter>(td::Ref<vm::Cell>());
  check("trampoline/null_body_error", rn.is_error());
}

}  // namespace

int run_abi_bridge_test() {
  g_fail = 0;
  test_increase_counter();
  test_jetton_wallet_data();
  test_hasvalue_union();
  test_structlabeled_union();
  test_adapter_direct();
  test_registry_policy();
  std::printf("ABI-BRIDGE-TEST %s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
  return g_fail == 0 ? 0 : 1;
}

}  // namespace mch
