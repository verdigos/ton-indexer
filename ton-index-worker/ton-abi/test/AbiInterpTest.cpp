// W8 TDD (T8.2): AbiInterp unpack/pack walker units.
//
// Two sources of ground truth:
//  (a) REAL committed fixtures (tolk_counter / lots-of-messages / lots-of-
//      wrappers) whose golden hex is already pinned in testdata/abi_vectors.json
//      (T0.2/W0) -- gives byte-exact confidence on scalars/prefixes/negative
//      ints/zero-field structs without hand-deriving wire bytes.
//  (b) A small hand-built ABI (like AbiLeavesPrefixTest's union_abi helper)
//      covering every other Ty kind the walker must drive: nullable, all 4
//      address kinds, cell/cellOf, bitsN/remaining, string, arrayOf/lispListOf/
//      tensor/shapedTuple (leaf byte-fidelity already proven at W6 -- these are
//      round-trip + to_json-shape checks, not fresh byte derivations), mapKV
//      (incl. non-standard-key reject), union (general/T-void/nullLiteral
//      shortcut), enum/alias passthrough, the custom-serializer registry gate,
//      and pack-side error paths (missing field, wrong kind).
//
// NB: vm::CellBuilder::finalize() is CONSUMING -- it resets the builder's
// internal bits/refs to empty as a side effect (see CellBuilder.cpp
// finalize_novm_nothrow). Every builder in this file is finalized EXACTLY
// ONCE into a named td::Ref<vm::Cell>, which is then reused for both the
// hash comparison and the CellSlice constructed from it. Calling finalize()
// twice on the same builder silently yields an EMPTY second cell -- do not
// reintroduce that pattern.

#include "doctest.h"

#pragma push_macro("CHECK")
#undef CHECK
#include "AbiInterp.h"
#include "AbiKernel.h"
#include "AbiLeavesRef.h"
#include "AbiLeavesScalar.h"
#include "AbiLoader.h"
#include "AbiValue.h"
#include "common/refint.h"
#include "vm/cells/Cell.h"
#include "vm/cells/CellBuilder.h"
#include "vm/cells/CellSlice.h"
#pragma pop_macro("CHECK")

#include <fstream>
#include <sstream>
#include <string>

#ifndef TON_ABI_FIXTURES_DIR
#error "TON_ABI_FIXTURES_DIR must be defined by CMake"
#endif

namespace {

using namespace ton_abi;

// ---- shared helpers (same idioms as AbiKernelTest / AbiLeavesPrefixTest) ---

std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(static_cast<bool>(in), "cannot open: " << path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

ContractABI load_fixture(const std::string &name) {
  std::string path = std::string(TON_ABI_FIXTURES_DIR) + "/" + name + ".abi.json";
  auto r = load_abi_from_json(read_file(path));
  REQUIRE_MESSAGE(r.is_ok(), name << ": " << (r.is_error() ? r.error().message().str() : ""));
  return r.move_as_ok();
}

ContractABI load_json(const std::string &json) {
  auto r = load_abi_from_json(json);
  REQUIRE_MESSAGE(r.is_ok(), (r.is_error() ? r.error().message().str() : ""));
  return r.move_as_ok();
}

td::RefInt256 Idec(const std::string &s) {
  return td::dec_string_to_int256(s);
}

// Finalize a builder EXACTLY ONCE (see the file-header note) and hand back
// both the cell and a slice over it, so callers never call .finalize() again.
struct Built {
  td::Ref<vm::Cell> cell;
  vm::CellSlice slice() const {
    return vm::load_cell_slice(cell);
  }
};
Built fin(vm::CellBuilder &cb) {
  return Built{cb.finalize()};
}

// The hand-built ABI for everything real fixtures don't exercise. Indices are
// referenced by name below (kTy* / kDecl*) for readability -- see the JSON.
const std::string kBigAbiJson = R"({
  "contract_name": "Big", "abi_schema_version": "1.0",
  "unique_types": [
    {"kind":"intN","n":8},
    {"kind":"uintN","n":16},
    {"kind":"bool"},
    {"kind":"cell"},
    {"kind":"address"},
    {"kind":"addressOpt"},
    {"kind":"addressExt"},
    {"kind":"addressAny"},
    {"kind":"nullable","inner_ty_idx":0},
    {"kind":"cellOf","inner_ty_idx":0},
    {"kind":"bitsN","n":5},
    {"kind":"remaining"},
    {"kind":"string"},
    {"kind":"arrayOf","inner_ty_idx":0},
    {"kind":"lispListOf","inner_ty_idx":0},
    {"kind":"tensor","items_ty_idx":[0,1]},
    {"kind":"shapedTuple","items_ty_idx":[0,1]},
    {"kind":"mapKV","key_ty_idx":1,"value_ty_idx":0},
    {"kind":"mapKV","key_ty_idx":4,"value_ty_idx":0},
    {"kind":"nullLiteral"},
    {"kind":"void"},
    {"kind":"StructRef","struct_name":"Point"},
    {"kind":"union","variants":[
      {"variant_ty_idx":0,"prefix_num":0,"prefix_len":2,"is_prefix_implicit":true},
      {"variant_ty_idx":1,"prefix_num":1,"prefix_len":2,"is_prefix_implicit":true},
      {"variant_ty_idx":21,"prefix_num":2,"prefix_len":2,"is_prefix_implicit":true}]},
    {"kind":"union","variants":[
      {"variant_ty_idx":0,"prefix_num":0,"prefix_len":1,"is_prefix_implicit":true},
      {"variant_ty_idx":20,"prefix_num":0,"prefix_len":0}]},
    {"kind":"union","variants":[
      {"variant_ty_idx":0,"prefix_num":0,"prefix_len":1,"is_prefix_implicit":true},
      {"variant_ty_idx":19,"prefix_num":1,"prefix_len":1,"is_prefix_implicit":true}]},
    {"kind":"EnumRef","enum_name":"Color"},
    {"kind":"AliasRef","alias_name":"MyInt"},
    {"kind":"StructRef","struct_name":"CustomStruct"},
    {"kind":"StructRef","struct_name":"TwoFields"},
    {"kind":"int"},
    {"kind":"mapKV","key_ty_idx":21,"value_ty_idx":0}
  ],
  "struct_instantiations": [], "alias_instantiations": [],
  "declarations": [
    {"kind":"struct","name":"Point","ty_idx":21,
     "fields":[{"name":"x","ty_idx":0},{"name":"y","ty_idx":1}]},
    {"kind":"struct","name":"CustomStruct","ty_idx":27,"fields":[],
     "custom_pack_unpack":{"pack_to_builder":true,"unpack_from_slice":true}},
    {"kind":"struct","name":"TwoFields","ty_idx":28,
     "fields":[{"name":"a","ty_idx":0},{"name":"b","ty_idx":1}]},
    {"kind":"enum","name":"Color","ty_idx":25,"encoded_as_ty_idx":0,
     "members":[{"name":"Red","value":"0"},{"name":"Blue","value":"1"}]},
    {"kind":"alias","name":"MyInt","ty_idx":26,"target_ty_idx":0}
  ],
  "storage": {}, "incoming_messages": [], "incoming_external": [], "outgoing_messages": [],
  "emitted_events": [], "get_methods": [], "thrown_errors": [],
  "compiler_name": "tolk", "compiler_version": "1.4.1"
})";

// ty_idx name table, matching kBigAbiJson's unique_types array order.
constexpr int kInt8 = 0, kUint16 = 1, kBool = 2, kCell = 3, kAddress = 4, kAddressOpt = 5, kAddressExt = 6,
              kAddressAny = 7, kNullableInt8 = 8, kCellOfInt8 = 9, kBits5 = 10, kRemaining = 11, kString = 12,
              kArrayInt8 = 13, kLispListInt8 = 14, kTensor = 15, kShapedTuple = 16, kMapU16I8 = 17,
              kMapAddrI8 = 18, kNullLiteral = 19, kVoid = 20, kPoint = 21, kUnionGeneral = 22, kUnionTVoid = 23,
              kUnionNullLit = 24, kEnumColor = 25, kAliasMyInt = 26, kCustomStruct = 27, kTwoFields = 28,
              kBareInt = 29, kMapNonStdKey = 30;

}  // namespace

// ===========================================================================
// (a) real fixtures: byte-exact via known golden hex (abi_vectors.json)
// ===========================================================================

TEST_CASE("AbiInterp: tolk_counter IncreaseCounter/ResetCounter round-trip (golden hex)") {
  ContractABI abi = load_fixture("tolk_counter");
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);
  const int kIncreaseCounter = 12, kResetCounter = 13;

  SUBCASE("IncreaseCounter {0,1} -> x{7E8764EF000000000000000000000001}") {
    vm::CellBuilder expect;
    REQUIRE(expect.store_long_bool(0x7E8764EFu, 32));
    REQUIRE(expect.store_long_bool(0, 64));
    REQUIRE(expect.store_long_bool(1, 32));
    Built ex = fin(expect);

    AbiValue v = AbiValue::make_struct("IncreaseCounter");
    v.add_field("queryId", AbiValue::make_int(Idec("0")));
    v.add_field("increaseBy", AbiValue::make_int(Idec("1")));
    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kIncreaseCounter, v, got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == ex.cell->get_hash());

    auto cs = ex.slice();
    auto r = unpack_value(ctx, kIncreaseCounter, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"({"$":"IncreaseCounter","queryId":"0","increaseBy":"1"})");
    CHECK(cs.size() == 0);
  }

  SUBCASE("IncreaseCounter {123456789,4294967295} -> x{7E8764EF00000000075BCD15FFFFFFFF}") {
    vm::CellBuilder expect;
    REQUIRE(expect.store_long_bool(0x7E8764EFu, 32));
    REQUIRE(expect.store_long_bool(123456789, 64));
    REQUIRE(expect.store_long_bool(0xFFFFFFFFu, 32));
    Built ex = fin(expect);

    AbiValue v = AbiValue::make_struct("IncreaseCounter");
    v.add_field("queryId", AbiValue::make_int(Idec("123456789")));
    v.add_field("increaseBy", AbiValue::make_int(Idec("4294967295")));
    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kIncreaseCounter, v, got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == ex.cell->get_hash());

    auto cs = ex.slice();
    auto r = unpack_value(ctx, kIncreaseCounter, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"({"$":"IncreaseCounter","queryId":"123456789","increaseBy":"4294967295"})");
  }

  SUBCASE("ResetCounter {18446744073709551615} -> x{3A752F06FFFFFFFFFFFFFFFF} (uint64 max)") {
    vm::CellBuilder expect;
    REQUIRE(expect.store_long_bool(0x3A752F06u, 32));
    REQUIRE(expect.store_long_bool(-1LL, 64));  // all-ones bit pattern == uint64 max
    Built ex = fin(expect);

    AbiValue v = AbiValue::make_struct("ResetCounter");
    v.add_field("queryId", AbiValue::make_int(Idec("18446744073709551615")));
    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kResetCounter, v, got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == ex.cell->get_hash());

    auto cs = ex.slice();
    auto r = unpack_value(ctx, kResetCounter, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"({"$":"ResetCounter","queryId":"18446744073709551615"})");
  }
}

TEST_CASE("AbiInterp: lots-of-messages IncreaseBy (negative int) / EmptyMsg (zero fields)") {
  ContractABI abi = load_fixture("lots-of-messages");
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);
  const int kIncreaseBy = 12, kEmptyMsg = 16;

  SUBCASE("IncreaseBy {-5,100} -> x{12345678FFFFFFFB00000064}") {
    vm::CellBuilder expect;
    REQUIRE(expect.store_long_bool(0x12345678u, 32));
    REQUIRE(expect.store_long_bool(-5, 32));
    REQUIRE(expect.store_long_bool(100, 32));
    Built ex = fin(expect);

    AbiValue v = AbiValue::make_struct("IncreaseBy");
    v.add_field("counter_id", AbiValue::make_int(Idec("-5")));
    v.add_field("inc_by", AbiValue::make_int(Idec("100")));
    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kIncreaseBy, v, got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == ex.cell->get_hash());

    auto cs = ex.slice();
    auto r = unpack_value(ctx, kIncreaseBy, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"({"$":"IncreaseBy","counter_id":"-5","inc_by":"100"})");
  }

  SUBCASE("EmptyMsg {} -> x{34567890}, zero-field struct dumps as just {\"$\":name}") {
    vm::CellBuilder expect;
    REQUIRE(expect.store_long_bool(0x34567890u, 32));
    Built ex = fin(expect);

    AbiValue v = AbiValue::make_struct("EmptyMsg");
    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kEmptyMsg, v, got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == ex.cell->get_hash());

    auto cs = ex.slice();
    auto r = unpack_value(ctx, kEmptyMsg, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"({"$":"EmptyMsg"})");
  }
}

TEST_CASE("AbiInterp: lots-of-wrappers literal golds (LotsOfWrappers.spec.ts transcriptions)") {
  ContractABI abi = load_fixture("lots-of-wrappers");
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);
  const int kMsgSinglePrefix32 = 163, kCounterIncrement = 167, kCounterDecrement = 185, kCounterReset0 = 186,
            kCounterResetTo = 187;

  SUBCASE("MsgSinglePrefix32 {80,800000000} -> x{8765432115042FAF0800} (amount1/2 are coins, not int32)") {
    vm::CellBuilder expect;
    REQUIRE(expect.store_long_bool(0x87654321u, 32));
    REQUIRE(store_coins(expect, Idec("80")).is_ok());
    REQUIRE(store_coins(expect, Idec("800000000")).is_ok());
    Built ex = fin(expect);

    AbiValue v = AbiValue::make_struct("MsgSinglePrefix32");
    v.add_field("amount1", AbiValue::make_int(Idec("80")));
    v.add_field("amount2", AbiValue::make_int(Idec("800000000")));
    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kMsgSinglePrefix32, v, got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == ex.cell->get_hash());
  }

  SUBCASE("CounterIncrement {123,78} round-trip, CounterDecrement {0,-38} round-trip (counter_id=int8, inc/dec_by=int32)") {
    vm::CellBuilder inc;
    REQUIRE(inc.store_long_bool(0x12345678u, 32));
    REQUIRE(inc.store_long_bool(123, 8));
    REQUIRE(inc.store_long_bool(78, 32));
    Built inc_b = fin(inc);
    auto cs = inc_b.slice();
    auto r = unpack_value(ctx, kCounterIncrement, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"({"$":"CounterIncrement","counter_id":"123","inc_by":"78"})");

    vm::CellBuilder dec;
    REQUIRE(dec.store_long_bool(0x23456789u, 32));
    REQUIRE(dec.store_long_bool(0, 8));
    REQUIRE(dec.store_long_bool(-38, 32));
    Built dec_b = fin(dec);
    auto cs2 = dec_b.slice();
    auto r2 = unpack_value(ctx, kCounterDecrement, cs2, "self");
    REQUIRE(r2.is_ok());
    CHECK(r2.ok().to_json() == R"({"$":"CounterDecrement","counter_id":"0","dec_by":"-38"})");
  }

  SUBCASE("CounterReset0 {0} / CounterResetTo {0,29874329774732} round-trip") {
    AbiValue v0 = AbiValue::make_struct("CounterReset0");
    v0.add_field("counter_id", AbiValue::make_int(Idec("0")));
    vm::CellBuilder got0;
    REQUIRE(pack_value(ctx, kCounterReset0, v0, got0, "self").is_ok());
    Built go0 = fin(got0);
    vm::CellBuilder expect0;
    REQUIRE(expect0.store_long_bool(0x34567890u, 32));
    REQUIRE(expect0.store_long_bool(0, 8));
    Built ex0 = fin(expect0);
    CHECK(go0.cell->get_hash() == ex0.cell->get_hash());

    AbiValue vr = AbiValue::make_struct("CounterResetTo");
    vr.add_field("counter_id", AbiValue::make_int(Idec("0")));
    vr.add_field("initial_value", AbiValue::make_int(Idec("29874329774732")));
    vm::CellBuilder gotr;
    REQUIRE(pack_value(ctx, kCounterResetTo, vr, gotr, "self").is_ok());
    Built gor = fin(gotr);
    auto cs = gor.slice();
    auto r = unpack_value(ctx, kCounterResetTo, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"({"$":"CounterResetTo","counter_id":"0","initial_value":"29874329774732"})");
  }
}

// ===========================================================================
// (b) hand-built ABI: everything else
// ===========================================================================

TEST_CASE("AbiInterp: nullable") {
  ContractABI abi = load_json(kBigAbiJson);
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);

  SUBCASE("present") {
    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(1, 1));
    REQUIRE(cb.store_long_bool(42, 8));
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, kNullableInt8, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"("42")");

    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kNullableInt8, r.ok(), got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == b.cell->get_hash());
  }
  SUBCASE("absent") {
    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(0, 1));
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, kNullableInt8, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == "null");

    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kNullableInt8, AbiValue::make_null(), got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == b.cell->get_hash());
  }
}

TEST_CASE("AbiInterp: address kinds") {
  ContractABI abi = load_json(kBigAbiJson);
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);

  auto store_hash = [](vm::CellBuilder &cb) {
    REQUIRE(cb.store_long_bool(0x1122334455667788LL, 64));
    REQUIRE(cb.store_long_bool(0x1122334455667788LL, 64));
    REQUIRE(cb.store_long_bool(0x1122334455667788LL, 64));
    REQUIRE(cb.store_long_bool(0x1122334455667788LL, 64));
  };

  SUBCASE("address (std only)") {
    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(2, 2));
    REQUIRE(cb.store_long_bool(0, 1));
    REQUIRE(cb.store_long_bool(0, 8));
    store_hash(cb);
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, kAddress, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().kind == AbiValueKind::Address);
    CHECK(r.ok().to_json().rfind("\"0:", 0) == 0);

    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kAddress, r.ok(), got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == b.cell->get_hash());
  }

  SUBCASE("addressOpt absent -> null") {
    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(0, 2));  // addr_none
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, kAddressOpt, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == "null");

    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kAddressOpt, AbiValue::make_null(), got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == b.cell->get_hash());
  }

  SUBCASE("addressAny none -> \"none\" (NOT null -- distinct from addressOpt)") {
    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(0, 2));
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, kAddressAny, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"("none")");

    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kAddressAny, r.ok(), got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == b.cell->get_hash());
  }

  SUBCASE("addressExt round-trip") {
    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(1, 2));
    REQUIRE(cb.store_long_bool(12, 9));
    REQUIRE(cb.store_long_bool(0xABC, 12));
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, kAddressExt, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"({"extern":{"bits":12,"value":"abc"}})");

    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kAddressExt, r.ok(), got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == b.cell->get_hash());
  }
}

TEST_CASE("AbiInterp: cellOf / bitsN / remaining / string") {
  ContractABI abi = load_json(kBigAbiJson);
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);

  SUBCASE("cellOf<int8>") {
    vm::CellBuilder inner;
    REQUIRE(inner.store_long_bool(42, 8));
    td::Ref<vm::Cell> inner_cell = inner.finalize();
    vm::CellBuilder outer;
    REQUIRE(outer.store_ref_bool(inner_cell));
    Built ou = fin(outer);
    auto cs = ou.slice();
    auto r = unpack_value(ctx, kCellOfInt8, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"({"ref":"42"})");

    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kCellOfInt8, r.ok(), got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == ou.cell->get_hash());
  }

  SUBCASE("bitsN(5)") {
    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(0b10110, 5));
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, kBits5, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"({"bits":"10110","refs":[]})");

    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kBits5, r.ok(), got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == b.cell->get_hash());
  }

  SUBCASE("remaining (same slice-ish dump as bitsN)") {
    vm::CellBuilder ref_cb;
    REQUIRE(ref_cb.store_long_bool(0xAB, 8));
    td::Ref<vm::Cell> ref_cell = ref_cb.finalize();
    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(0b11, 2));
    REQUIRE(cb.store_ref_bool(ref_cell));
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, kRemaining, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().kind == AbiValueKind::Bits);

    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kRemaining, r.ok(), got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == b.cell->get_hash());
  }

  SUBCASE("string") {
    vm::CellBuilder cb;
    REQUIRE(store_string(cb, "hello").is_ok());
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, kString, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"("hello")");

    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kString, r.ok(), got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == b.cell->get_hash());
  }
}

TEST_CASE("AbiInterp: arrayOf / lispListOf / tensor / shapedTuple (round-trip; leaf byte-fidelity proven at W6)") {
  ContractABI abi = load_json(kBigAbiJson);
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);

  SUBCASE("arrayOf<int8>") {
    std::vector<AbiValue> items;
    items.push_back(AbiValue::make_int(Idec("1")));
    items.push_back(AbiValue::make_int(Idec("2")));
    items.push_back(AbiValue::make_int(Idec("3")));
    AbiValue v = AbiValue::make_list(std::move(items));
    vm::CellBuilder cb;
    REQUIRE(pack_value(ctx, kArrayInt8, v, cb, "self").is_ok());
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, kArrayInt8, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"(["1","2","3"])");
  }

  SUBCASE("lispListOf<int8>") {
    std::vector<AbiValue> items;
    items.push_back(AbiValue::make_int(Idec("5")));
    items.push_back(AbiValue::make_int(Idec("6")));
    AbiValue v = AbiValue::make_list(std::move(items));
    vm::CellBuilder cb;
    REQUIRE(pack_value(ctx, kLispListInt8, v, cb, "self").is_ok());
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, kLispListInt8, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"(["5","6"])");
  }

  SUBCASE("tensor(int8,uint16) and shapedTuple(int8,uint16) -- same JSON array shape") {
    for (int ty_idx : {kTensor, kShapedTuple}) {
      std::vector<AbiValue> items;
      items.push_back(AbiValue::make_int(Idec("7")));
      items.push_back(AbiValue::make_int(Idec("8")));
      AbiValue v = AbiValue::make_list(std::move(items));
      vm::CellBuilder cb;
      REQUIRE(pack_value(ctx, ty_idx, v, cb, "self").is_ok());
      Built b = fin(cb);
      auto cs = b.slice();
      auto r = unpack_value(ctx, ty_idx, cs, "self");
      REQUIRE(r.is_ok());
      CHECK(r.ok().to_json() == R"(["7","8"])");
    }
  }
}

TEST_CASE("AbiInterp: mapKV") {
  ContractABI abi = load_json(kBigAbiJson);
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);

  SUBCASE("uint16 key: round-trip yields ascending wire order regardless of input order") {
    std::vector<std::pair<AbiValue, AbiValue>> entries;
    entries.emplace_back(AbiValue::make_int(Idec("5")), AbiValue::make_int(Idec("10")));
    entries.emplace_back(AbiValue::make_int(Idec("1")), AbiValue::make_int(Idec("20")));
    AbiValue v = AbiValue::make_map(std::move(entries));
    vm::CellBuilder cb;
    REQUIRE(pack_value(ctx, kMapU16I8, v, cb, "self").is_ok());
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, kMapU16I8, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"([["1","20"],["5","10"]])");
  }

  SUBCASE("address key: single entry round-trip") {
    AbiAddress a;
    a.kind = AbiAddressKind::Std;
    a.workchain = 0;
    vm::CellBuilder hb;
    REQUIRE(hb.store_long_bool(0x1122334455667788LL, 64));
    REQUIRE(hb.store_long_bool(0x1122334455667788LL, 64));
    REQUIRE(hb.store_long_bool(0x1122334455667788LL, 64));
    REQUIRE(hb.store_long_bool(0x1122334455667788LL, 64));
    Built hbuilt = fin(hb);
    auto hcs = hbuilt.slice();
    REQUIRE(hcs.prefetch_bits_to(a.hash));

    std::vector<std::pair<AbiValue, AbiValue>> entries;
    entries.emplace_back(AbiValue::make_address(a), AbiValue::make_int(Idec("99")));
    AbiValue v = AbiValue::make_map(std::move(entries));
    vm::CellBuilder cb;
    REQUIRE(pack_value(ctx, kMapAddrI8, v, cb, "self").is_ok());
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, kMapAddrI8, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().kind == AbiValueKind::Map);
    REQUIRE(r.ok().map_entries.size() == 1);
    CHECK(r.ok().map_entries[0].second.to_json() == R"("99")");
  }

  SUBCASE("non-standard key type rejects, both directions") {
    vm::CellBuilder cb1;
    Built b1 = fin(cb1);
    auto cs = b1.slice();
    CHECK(unpack_value(ctx, kMapNonStdKey, cs, "self").is_error());

    vm::CellBuilder cb2;
    CHECK(pack_value(ctx, kMapNonStdKey, AbiValue::make_map({}), cb2, "self").is_error());
  }
}

TEST_CASE("AbiInterp: union general dispatch (primitive variant wraps, struct-labeled variant doesn't)") {
  ContractABI abi = load_json(kBigAbiJson);
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);

  SUBCASE("dispatch to int8 (has_value_field=true) -> {\"$\":\"int8\",\"value\":...}") {
    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(0, 2));  // prefix "00"
    REQUIRE(cb.store_long_bool(7, 8));
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, kUnionGeneral, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"({"$":"int8","value":"7"})");

    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kUnionGeneral, r.ok(), got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == b.cell->get_hash());
  }

  SUBCASE("dispatch to Point (has_value_field=false, struct carries its own $) -> no wrapper") {
    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(2, 2));  // prefix "10"
    REQUIRE(cb.store_long_bool(3, 8));  // x
    REQUIRE(cb.store_long_bool(4, 16)); // y
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, kUnionGeneral, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().kind == AbiValueKind::Struct);
    CHECK(r.ok().to_json() == R"({"$":"Point","x":"3","y":"4"})");

    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kUnionGeneral, r.ok(), got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == b.cell->get_hash());
  }

  SUBCASE("non-existing union variant '$' on pack -> error") {
    AbiValue bogus = AbiValue::make_union("nope", AbiValue::make_int(Idec("1")));
    vm::CellBuilder cb;
    CHECK(pack_value(ctx, kUnionGeneral, bogus, cb, "self").is_error());
  }
}

TEST_CASE("AbiInterp: union T|void") {
  ContractABI abi = load_json(kBigAbiJson);
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);

  SUBCASE("empty slice -> void, and packing Void writes nothing back") {
    vm::CellBuilder empty;
    Built eb = fin(empty);
    auto cs = eb.slice();
    auto r = unpack_value(ctx, kUnionTVoid, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().kind == AbiValueKind::Void);
    CHECK(r.ok().to_json() == R"({"$":"void"})");

    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kUnionTVoid, r.ok(), got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == eb.cell->get_hash());
  }

  SUBCASE("non-empty -> T (int8), implicit prefix eaten, round-trip") {
    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(0, 1));
    REQUIRE(cb.store_long_bool(9, 8));
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, kUnionTVoid, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"({"$":"int8","value":"9"})");

    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kUnionTVoid, r.ok(), got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == b.cell->get_hash());
  }
}

TEST_CASE("AbiInterp: union with a nullLiteral variant -- hasNull pack shortcut") {
  ContractABI abi = load_json(kBigAbiJson);
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);

  SUBCASE("dispatch to nullLiteral variant -> plain JSON null, not wrapped") {
    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(1, 1));  // prefix "1" -> nullLiteral variant
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, kUnionNullLit, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().kind == AbiValueKind::Null);
    CHECK(r.ok().to_json() == "null");
  }

  SUBCASE("packing a bare Null value uses the hasNull shortcut (unconditional prefix write)") {
    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kUnionNullLit, AbiValue::make_null(), got, "self").is_ok());
    Built go = fin(got);
    vm::CellBuilder expect;
    REQUIRE(expect.store_long_bool(1, 1));
    Built ex = fin(expect);
    CHECK(go.cell->get_hash() == ex.cell->get_hash());
  }

  SUBCASE("dispatch to int8 variant -> wrapped (has_value_field=true)") {
    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(0, 1));
    REQUIRE(cb.store_long_bool(11, 8));
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, kUnionNullLit, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"({"$":"int8","value":"11"})");
  }
}

TEST_CASE("AbiInterp: enum passthrough (no custom) and alias passthrough (no custom)") {
  ContractABI abi = load_json(kBigAbiJson);
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);

  SUBCASE("enum decodes as the raw encoded_as_ty_idx value (decimal, no member-name validation)") {
    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(1, 8));
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, kEnumColor, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"("1")");

    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kEnumColor, r.ok(), got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == b.cell->get_hash());
  }

  SUBCASE("alias proxies transparently to its target") {
    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(-7, 8));
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, kAliasMyInt, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"("-7")");

    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kAliasMyInt, r.ok(), got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == b.cell->get_hash());
  }
}

TEST_CASE("AbiInterp: custom-serializer registry gate (§0.17), independently-optional pack/unpack") {
  ContractABI abi = load_json(kBigAbiJson);
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();

  SUBCASE("declared custom but nothing registered -> 'not registered' error both directions") {
    AbiInterpCtx ctx(kernel);
    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(0, 8));
    Built b = fin(cb);
    auto cs = b.slice();
    auto ur = unpack_value(ctx, kCustomStruct, cs, "self");
    REQUIRE(ur.is_error());
    CHECK(ur.error().message().str().find("not registered") != std::string::npos);

    vm::CellBuilder cb2;
    auto pr = pack_value(ctx, kCustomStruct, AbiValue::make_struct("CustomStruct"), cb2, "self");
    CHECK(pr.is_error());
  }

  SUBCASE("registered unpack-only -> unpack uses it, pack still errors 'not registered'") {
    AbiInterpCtx ctx(kernel);
    REQUIRE(register_abi_custom_dynamic(
                ctx, "CustomStruct", nullptr,
                [](vm::CellSlice &cs) -> td::Result<AbiValue> {
                  TRY_RESULT(v, load_uint(cs, 8));
                  return AbiValue::make_int(std::move(v));
                })
                .is_ok());

    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(99, 8));
    Built b = fin(cb);
    auto cs = b.slice();
    auto ur = unpack_value(ctx, kCustomStruct, cs, "self");
    REQUIRE(ur.is_ok());
    CHECK(ur.ok().to_json() == R"("99")");

    vm::CellBuilder cb2;
    CHECK(pack_value(ctx, kCustomStruct, AbiValue::make_struct("CustomStruct"), cb2, "self").is_error());
  }

  SUBCASE("registered both -> both directions use the custom fns") {
    AbiInterpCtx ctx(kernel);
    REQUIRE(register_abi_custom_dynamic(
                ctx, "CustomStruct",
                [](const AbiValue &, vm::CellBuilder &cb) -> td::Status { return store_uint(cb, Idec("171"), 8); },
                [](vm::CellSlice &cs) -> td::Result<AbiValue> {
                  TRY_RESULT(v, load_uint(cs, 8));
                  return AbiValue::make_int(std::move(v));
                })
                .is_ok());

    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, kCustomStruct, AbiValue::make_struct("CustomStruct"), got, "self").is_ok());
    Built go = fin(got);
    auto cs = go.slice();
    auto r = unpack_value(ctx, kCustomStruct, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"("171")");  // 0xAB
  }
}

TEST_CASE("AbiInterp: pack error paths") {
  ContractABI abi = load_json(kBigAbiJson);
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);

  SUBCASE("missing struct field") {
    AbiValue v = AbiValue::make_struct("TwoFields");
    v.add_field("a", AbiValue::make_int(Idec("1")));  // "b" omitted
    vm::CellBuilder cb;
    auto r = pack_value(ctx, kTwoFields, v, cb, "self");
    REQUIRE(r.is_error());
    CHECK(r.error().message().str().find("missing field") != std::string::npos);
  }

  SUBCASE("wrong AbiValue kind for a scalar field") {
    vm::CellBuilder cb;
    auto r = pack_value(ctx, kInt8, AbiValue::make_bool(true), cb, "self");
    REQUIRE(r.is_error());
    CHECK(r.error().message().str().find("expected Int") != std::string::npos);
  }

  SUBCASE("non-serializable bare 'int' type rejects both directions") {
    vm::CellBuilder cb1;
    Built b1 = fin(cb1);
    auto cs = b1.slice();
    CHECK(unpack_value(ctx, kBareInt, cs, "self").is_error());

    vm::CellBuilder cb2;
    CHECK(pack_value(ctx, kBareInt, AbiValue::make_int(Idec("1")), cb2, "self").is_error());
  }
}

TEST_CASE("AbiInterp: unpack_message_body rejects an exotic/special root cell") {
  ContractABI abi = load_json(kBigAbiJson);
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);

  vm::CellBuilder inner;
  REQUIRE(inner.store_long_bool(0, 8));
  td::Ref<vm::Cell> special_cell = vm::CellBuilder::create_merkle_proof(inner.finalize());
  bool is_special = false;
  vm::load_cell_slice_special(special_cell, is_special);
  REQUIRE(is_special);

  auto r = unpack_message_body(ctx, kInt8, special_cell);
  CHECK(r.is_error());
}
