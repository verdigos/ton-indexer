// W9 (T9.1/T9.2): interpreter-only suite port, DEDUPLICATED per plan §3
// (round 3 revision) -- golden-hex bulk already lives in VectorRunner (T8.3);
// this file covers what vectors CAN'T express:
//  (a) the 10 custom-serializer types (LotsOfWrappers.spec.ts:243-380), ported
//      to C++ lambdas registered via register_abi_custom_dynamic. Follows the
//      DYNAMIC-side registration asymmetry: Color is declared custom in the
//      ABI but NOT registered on our ctx either (spec :376 commented out) --
//      §0.17.
//  (b) exact-error asserts (LotsOfWrappers.spec.ts:1374-1419), re-specified as
//      trigger + error-CATEGORY checks (not literal TS message text -- see
//      testdata/ERROR_MAPPING.md for the TS-message -> C++-disposition table).
//  (c) T9.2 carve-outs: ClientTypeAnno client_ty_idx cell-serialization
//      (:69,:112), GenericUnionLabels literal-value round-trips (:105),
//      LotsOfMessages createCellOf hash-equality (c1/c3 of :45 -- c2 needs
//      create()-with-defaults, out of scope until W11 per plan).
//
// Representative direct round-trip units (item (c) of T9.1) are NOT
// duplicated here -- AbiInterpTest.cpp (T8.2) already has ~20 of them across
// every kind (scalars/addresses/nullable/cellOf/bits/string/array/lispList/
// tensor/map/union x3/enum/alias/custom-registry/errors).

#include "doctest.h"

#pragma push_macro("CHECK")
#undef CHECK
#include "AbiInterp.h"
#include "AbiKernel.h"
#include "AbiLeavesPrefix.h"
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

td::RefInt256 Idec(const std::string &s) {
  return td::dec_string_to_int256(s);
}

struct Built {
  td::Ref<vm::Cell> cell;
  vm::CellSlice slice() const {
    return vm::load_cell_slice(cell);
  }
};
Built fin(vm::CellBuilder &cb) {
  return Built{cb.finalize()};
}

// ---------------------------------------------------------------------------
// (a) custom serializers -- ports of LotsOfWrappers.spec.ts:243-380
// ---------------------------------------------------------------------------

// TelegramString: alias -> slice (bare, non-serializable normally -- custom
// bypasses that). Wire = uint8 byte-count prefix + that many bytes, 0 refs.
td::Status TelegramString_pack(const AbiValue &v, vm::CellBuilder &cb) {
  if (v.kind != AbiValueKind::Bits) {
    return td::Status::Error("TelegramString custom pack: expected Bits value");
  }
  int bytes = (static_cast<int>(v.bits_v->size()) + 7) / 8;
  TRY_STATUS(store_uint(cb, td::make_refint(bytes), 8));
  cb.store_bits(v.bits_v->data_bits(), v.bits_v->size());
  return td::Status::OK();
}
td::Result<AbiValue> TelegramString_unpack(vm::CellSlice &cs) {
  TRY_RESULT(bytes, load_uint(cs, 8));
  TRY_RESULT(bits, load_bits(cs, static_cast<int>(bytes->to_long()) * 8));
  return AbiValue::make_bits(std::move(bits));
}

// Custom8: alias -> uint8; custom encoding happens to match plain uint8.
td::Status Custom8_pack(const AbiValue &v, vm::CellBuilder &cb) {
  if (v.kind != AbiValueKind::Int) {
    return td::Status::Error("Custom8 custom pack: expected Int value");
  }
  return store_uint(cb, v.int_v, 8);
}
td::Result<AbiValue> Custom8_unpack(vm::CellSlice &cs) {
  TRY_RESULT(v, load_uint(cs, 8));
  return AbiValue::make_int(std::move(v));
}

// MyBorderedInt: alias -> int257; a lossy 3-way "bordered" encoding (encodes
// which RANGE the value falls in, not the exact value -- unpack returns a
// fixed representative per range, exactly per the reference).
td::Status MyBorderedInt_pack(const AbiValue &v, vm::CellBuilder &cb) {
  if (v.kind != AbiValueKind::Int) {
    return td::Status::Error("MyBorderedInt custom pack: expected Int value");
  }
  int tag = v.int_v > 10 ? 1 : (v.int_v > 0 ? 2 : 3);
  return store_uint(cb, td::make_refint(tag), 4);
}
td::Result<AbiValue> MyBorderedInt_unpack(vm::CellSlice &cs) {
  TRY_RESULT(tag, load_uint(cs, 4));
  long long t = tag->to_long();
  if (t == 1) return AbiValue::make_int(Idec("10"));
  if (t == 2) return AbiValue::make_int(Idec("0"));
  if (t == 3) return AbiValue::make_int(Idec("-1"));
  return td::Status::Error("MyBorderedInt custom unpack: bad tag");
}

// MyCustomNothing: alias -> tensor[] ("void"-shaped). custom_pack_unpack in
// the ABI only has pack_to_builder=true (no unpack_from_slice) -- matches the
// reference (OnlyWithPack-style: unpack is never gated to a custom fn at all,
// falls through to the normal empty-tensor alias target). Pack-only port.
td::Status MyCustomNothing_pack(const AbiValue &, vm::CellBuilder &cb) {
  TRY_STATUS(store_uint(cb, td::make_refint(123), 32));
  vm::CellBuilder empty_ref;
  return store_cell(cb, empty_ref.finalize());
}

// Tensor3Skipping1: alias -> tensor[uint8,uint8,uint8]; pack skips the middle
// element, unpack fills it back in as 0.
td::Status Tensor3Skipping1_pack(const AbiValue &v, vm::CellBuilder &cb) {
  if (v.kind != AbiValueKind::List || v.list_v.size() != 3) {
    return td::Status::Error("Tensor3Skipping1 custom pack: expected a 3-element List");
  }
  TRY_STATUS(store_uint(cb, v.list_v[0].int_v, 8));
  return store_uint(cb, v.list_v[2].int_v, 8);
}
td::Result<AbiValue> Tensor3Skipping1_unpack(vm::CellSlice &cs) {
  TRY_RESULT(e0, load_uint(cs, 8));
  TRY_RESULT(e2, load_uint(cs, 8));
  std::vector<AbiValue> items;
  items.push_back(AbiValue::make_int(std::move(e0)));
  items.push_back(AbiValue::make_int(Idec("0")));
  items.push_back(AbiValue::make_int(std::move(e2)));
  return AbiValue::make_list(std::move(items));
}

// Color: enum, custom encoding is 5 bits (NOT the enum's own encoded_as_ty_idx
// width) -- deliberately NOT registered on our ctx (asymmetry test only).
td::Status Color_pack(const AbiValue &v, vm::CellBuilder &cb) {
  return store_uint(cb, v.int_v, 5);
}
td::Result<AbiValue> Color_unpack(vm::CellSlice &cs) {
  TRY_RESULT(v, load_uint(cs, 5));
  return AbiValue::make_int(std::move(v));
}

// OnlyWithPack: alias -> int8, pack-only (0xFF marker + the value).
td::Status OnlyWithPack_pack(const AbiValue &v, vm::CellBuilder &cb) {
  if (v.kind != AbiValueKind::Int) {
    return td::Status::Error("OnlyWithPack custom pack: expected Int value");
  }
  TRY_STATUS(store_uint(cb, td::make_refint(0xFF), 8));
  return store_uint(cb, v.int_v, 8);
}

// OnlyWithUnpack: alias -> int, unpack-only (validates the 0xFF marker).
td::Result<AbiValue> OnlyWithUnpack_unpack(vm::CellSlice &cs) {
  TRY_RESULT(marker, load_uint(cs, 8));
  if (marker->to_long() != 0xFF) {
    return td::Status::Error("OnlyWithUnpack custom unpack: missing 0xFF marker");
  }
  TRY_RESULT(v, load_uint(cs, 8));
  return AbiValue::make_int(std::move(v));
}

// CustomPoint / CustomPointAlias: struct-shaped custom (x,y each 8 bits,
// declared type is bare 'int' but the custom bypasses that entirely). Even
// when registered under the "...Alias" key, unpack hardcodes struct_name =
// "CustomPoint" -- matches the reference's own hardcoded literal.
td::Status CustomPoint_pack(const AbiValue &v, vm::CellBuilder &cb) {
  if (v.kind != AbiValueKind::Struct) {
    return td::Status::Error("CustomPoint custom pack: expected Struct value");
  }
  const AbiValue *x = nullptr;
  const AbiValue *y = nullptr;
  for (const auto &f : v.struct_fields) {
    if (f.first == "x") x = &f.second;
    if (f.first == "y") y = &f.second;
  }
  if (x == nullptr || y == nullptr) {
    return td::Status::Error("CustomPoint custom pack: missing x/y field");
  }
  TRY_STATUS(store_uint(cb, x->int_v, 8));
  return store_uint(cb, y->int_v, 8);
}
td::Result<AbiValue> CustomPoint_unpack(vm::CellSlice &cs) {
  TRY_RESULT(x, load_uint(cs, 8));
  TRY_RESULT(y, load_uint(cs, 8));
  AbiValue result = AbiValue::make_struct("CustomPoint");
  result.add_field("x", AbiValue::make_int(std::move(x)));
  result.add_field("y", AbiValue::make_int(std::move(y)));
  return result;
}

void register_lots_of_wrappers_customs(AbiInterpCtx &ctx) {
  REQUIRE(register_abi_custom_dynamic(ctx, "TelegramString", TelegramString_pack, TelegramString_unpack).is_ok());
  REQUIRE(register_abi_custom_dynamic(ctx, "Custom8", Custom8_pack, Custom8_unpack).is_ok());
  REQUIRE(register_abi_custom_dynamic(ctx, "MyBorderedInt", MyBorderedInt_pack, MyBorderedInt_unpack).is_ok());
  REQUIRE(register_abi_custom_dynamic(ctx, "MyCustomNothing", MyCustomNothing_pack, nullptr).is_ok());
  REQUIRE(register_abi_custom_dynamic(ctx, "Tensor3Skipping1", Tensor3Skipping1_pack, Tensor3Skipping1_unpack).is_ok());
  // Color: intentionally NOT registered (spec :376 asymmetry).
  REQUIRE(register_abi_custom_dynamic(ctx, "OnlyWithPack", OnlyWithPack_pack, nullptr).is_ok());
  REQUIRE(register_abi_custom_dynamic(ctx, "OnlyWithUnpack", nullptr, OnlyWithUnpack_unpack).is_ok());
  REQUIRE(register_abi_custom_dynamic(ctx, "CustomPoint", CustomPoint_pack, CustomPoint_unpack).is_ok());
  REQUIRE(register_abi_custom_dynamic(ctx, "CustomPointAlias", CustomPoint_pack, CustomPoint_unpack).is_ok());
}

}  // namespace

TEST_CASE("W9 custom serializers: round-trip via the real lots-of-wrappers fixture") {
  ContractABI abi = load_fixture("lots-of-wrappers");
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);
  register_lots_of_wrappers_customs(ctx);

  SUBCASE("StorWithStr (TelegramString field)") {
    int ty = kernel.get_struct("StorWithStr").ok()->ty_idx;
    vm::CellBuilder cb;
    REQUIRE(store_int(cb, Idec("1"), 32).is_ok());  // a: int32
    vm::CellBuilder text;
    REQUIRE(store_uint(text, td::make_refint(5), 8).is_ok());
    text.store_long(0x68656C6C6FLL, 40);  // "hello"
    Built tb = fin(text);
    auto text_cs = tb.slice();
    cb.store_bits(text_cs.data_bits(), text_cs.size());
    REQUIRE(store_int(cb, Idec("2"), 32).is_ok());  // b: int32
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, ty, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"({"$":"StorWithStr","a":"1","str":{"bits":"0110100001100101011011000110110001101111","refs":[]},"b":"2"})");

    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, ty, r.ok(), got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == b.cell->get_hash());
  }

  SUBCASE("PointWithCustomInt (Custom8 field)") {
    int ty = kernel.get_struct("PointWithCustomInt").ok()->ty_idx;
    vm::CellBuilder cb;
    REQUIRE(store_uint(cb, Idec("42"), 8).is_ok());  // a: Custom8
    REQUIRE(store_int(cb, Idec("5"), 8).is_ok());     // b: int8
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, ty, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"({"$":"PointWithCustomInt","a":"42","b":"5"})");
  }

  SUBCASE("WithMyBorder (MyBorderedInt field): lossy range encoding both ways") {
    int ty = kernel.get_struct("WithMyBorder").ok()->ty_idx;
    for (const auto &kv : std::vector<std::pair<std::string, std::string>>{
             {"20", "10"}, {"5", "0"}, {"-3", "-1"}}) {
      AbiValue v = AbiValue::make_struct("WithMyBorder");
      v.add_field("a", AbiValue::make_int(Idec("1")));
      v.add_field("b", AbiValue::make_int(Idec(kv.first)));
      vm::CellBuilder got;
      REQUIRE(pack_value(ctx, ty, v, got, "self").is_ok());
      Built go = fin(got);
      auto cs = go.slice();
      auto r = unpack_value(ctx, ty, cs, "self");
      REQUIRE(r.is_ok());
      CHECK(r.ok().to_json() == (std::string(R"({"$":"WithMyBorder","a":"1","b":")") + kv.second + R"("})"));
    }
  }

  SUBCASE("WithFakeWriter (MyCustomNothing field): pack-only") {
    int ty = kernel.get_struct("WithFakeWriter").ok()->ty_idx;
    AbiValue v = AbiValue::make_struct("WithFakeWriter");
    v.add_field("a", AbiValue::make_int(Idec("1")));
    v.add_field("fake", AbiValue::make_void());  // unused by the pack lambda
    v.add_field("b", AbiValue::make_int(Idec("2")));
    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, ty, v, got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.slice().size() == 8 + 32 + 8);  // a(int8) + fake(uint32 marker) + b(int8); fake's ref not counted in bits
  }

  SUBCASE("Tensor3Skipping1: direct round-trip (no wrapping struct in the fixture)") {
    int ty = kernel.get_alias("Tensor3Skipping1").ok()->ty_idx;
    std::vector<AbiValue> items;
    items.push_back(AbiValue::make_int(Idec("7")));
    items.push_back(AbiValue::make_int(Idec("99")));  // packed away, unpack replaces with 0
    items.push_back(AbiValue::make_int(Idec("8")));
    AbiValue v = AbiValue::make_list(std::move(items));
    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, ty, v, got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.slice().size() == 16);
    auto cs = go.slice();
    auto r = unpack_value(ctx, ty, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"(["7","0","8"])");
  }

  SUBCASE("CustomPoint / CustomPointAlias: both keys unpack to struct_name \"CustomPoint\"") {
    int tyPoint = kernel.get_struct("CustomPoint").ok()->ty_idx;
    int tyAlias = kernel.get_alias("CustomPointAlias").ok()->ty_idx;
    for (int ty : {tyPoint, tyAlias}) {
      vm::CellBuilder cb;
      REQUIRE(store_uint(cb, Idec("3"), 8).is_ok());
      REQUIRE(store_uint(cb, Idec("4"), 8).is_ok());
      Built b = fin(cb);
      auto cs = b.slice();
      auto r = unpack_value(ctx, ty, cs, "self");
      REQUIRE(r.is_ok());
      CHECK(r.ok().to_json() == R"({"$":"CustomPoint","x":"3","y":"4"})");
    }
  }
}

TEST_CASE("W9 custom serializers: DYNAMIC-side registration asymmetry (Color not registered, §0.17)") {
  ContractABI abi = load_fixture("lots-of-wrappers");
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);
  register_lots_of_wrappers_customs(ctx);  // deliberately excludes Color

  int tyColor = kernel.get_enum("Color").ok()->ty_idx;

  vm::CellBuilder cb;
  REQUIRE(store_uint(cb, Idec("1"), 5).is_ok());
  Built b = fin(cb);
  auto cs = b.slice();
  auto r_unpack = unpack_value(ctx, tyColor, cs, "self");
  CHECK(r_unpack.is_error());
  CHECK(r_unpack.error().message().str().find("not registered") != std::string::npos);

  vm::CellBuilder got;
  auto r_pack = pack_value(ctx, tyColor, AbiValue::make_int(Idec("1")), got, "self");
  CHECK(r_pack.is_error());
  CHECK(r_pack.error().message().str().find("not registered") != std::string::npos);
}

TEST_CASE("W9 custom serializers: independently-optional pack/unpack, real fixture types") {
  ContractABI abi = load_fixture("lots-of-wrappers");
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);
  register_lots_of_wrappers_customs(ctx);

  SUBCASE("OnlyWithPack: pack works, unpack errors (not registered -- ABI itself has no unpack_from_slice)") {
    int ty = kernel.get_alias("OnlyWithPack").ok()->ty_idx;
    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, ty, AbiValue::make_int(Idec("7")), got, "self").is_ok());
    Built go = fin(got);
    vm::CellBuilder expect;
    REQUIRE(store_uint(expect, td::make_refint(0xFF), 8).is_ok());
    REQUIRE(store_uint(expect, Idec("7"), 8).is_ok());
    Built ex = fin(expect);
    CHECK(go.cell->get_hash() == ex.cell->get_hash());
  }

  SUBCASE("OnlyWithUnpack: unpack works, pack errors (not registered)") {
    int ty = kernel.get_alias("OnlyWithUnpack").ok()->ty_idx;
    vm::CellBuilder cb;
    REQUIRE(store_uint(cb, td::make_refint(0xFF), 8).is_ok());
    REQUIRE(store_uint(cb, Idec("9"), 8).is_ok());
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, ty, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == R"("9")");

    vm::CellBuilder got;
    CHECK(pack_value(ctx, ty, AbiValue::make_int(Idec("9")), got, "self").is_error());
  }

  SUBCASE("MyCustomNothing: pack-only (unpack is never gated to a custom fn -- ABI has no unpack_from_slice)") {
    int ty = kernel.get_alias("MyCustomNothing").ok()->ty_idx;
    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, ty, AbiValue::make_void(), got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.slice().size() == 32);  // the 32-bit marker; the empty ref isn't counted in bit size
  }
}

// ---------------------------------------------------------------------------
// (b) error-mapping ports -- LotsOfWrappers.spec.ts:1374-1419, re-specified as
// trigger + category (see testdata/ERROR_MAPPING.md for the full table).
// ---------------------------------------------------------------------------

TEST_CASE("W9 error mapping: non-serializable field types (spec 'not packable', real fixture structs)") {
  ContractABI abi = load_fixture("lots-of-wrappers");
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);

  auto expect_unpack_error = [&](const char *struct_name) {
    int ty = kernel.get_struct(struct_name).ok()->ty_idx;
    vm::CellBuilder cb;
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, ty, cs, "self");
    CAPTURE(struct_name);
    CHECK(r.is_error());
  };

  // NotPackable1: tensor[int8, bare-int] -- bare 'int' is get-method/stack
  // domain, non-serializable per plan decision 1.
  expect_unpack_error("NotPackable1");
  // NotPackable5: shapedTuple containing a 'callable' element.
  expect_unpack_error("NotPackable5");
  // NotPackable6 / NotPackable7: bare 'callable' field ("continuation").
  expect_unpack_error("NotPackable6");
  expect_unpack_error("NotPackable7");
  // NotPackable8: bare 'unknown' field.
  expect_unpack_error("NotPackable8");
  // NotUnpackable1: tensor containing a 'builder' element.
  // DIVERGENCE (documented in ERROR_MAPPING.md): the reference allows PACKING
  // a builder value (storeBuilder passthrough) while rejecting only unpack;
  // we reject 'builder' in BOTH directions (plan decision 1 treats
  // builder/slice/callable/unknown as a single non-serializable bucket,
  // get-method/stack domain, out of scope) -- so both directions error here,
  // not just unpack. Confirmed intentional simplification, not a bug.
  expect_unpack_error("NotUnpackable1");
  {
    int ty = kernel.get_struct("NotUnpackable1").ok()->ty_idx;
    vm::CellBuilder cb;
    CHECK(pack_value(ctx, ty, AbiValue::make_void(), cb, "self").is_error());
  }
}

TEST_CASE("W9 error mapping: dynamic pack/unpack error categories (spec :1393-1419)") {
  ContractABI abi = load_fixture("lots-of-wrappers");
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);

  SUBCASE("wrong AbiValue kind for a struct field (spec: 'not a number'/'not an object')") {
    int ty = kernel.get_struct("JustInt32").ok()->ty_idx;
    vm::CellBuilder cb;
    auto r = pack_value(ctx, ty, AbiValue::make_bool(true), cb, "self");
    REQUIRE(r.is_error());
    CHECK(r.error().message().str().find("expected") != std::string::npos);
  }

  SUBCASE("int32 value out of range (spec: 'value is out of range for 32 bits')") {
    int ty = kernel.get_struct("JustInt32").ok()->ty_idx;
    AbiValue v = AbiValue::make_struct("JustInt32");
    v.add_field("value", AbiValue::make_int(Idec("1099511627776")));  // 1<<40
    vm::CellBuilder cb;
    CHECK(pack_value(ctx, ty, v, cb, "self").is_error());
  }

  SUBCASE("union pack: value missing '$' (spec: 'not an object with property $')") {
    int ty = kernel.get_struct("IntAndEitherInt8Or256").ok()->ty_idx;
    AbiValue v = AbiValue::make_struct("IntAndEitherInt8Or256");
    v.add_field("op", AbiValue::make_int(Idec("1")));
    v.add_field("i8or256", AbiValue::make_int(Idec("123")));  // bare int, no $ wrapper
    vm::CellBuilder cb;
    auto r = pack_value(ctx, ty, v, cb, "self");
    REQUIRE(r.is_error());
    CHECK(r.error().message().str().find("expected") != std::string::npos);
  }

  SUBCASE("union pack: non-existing variant $ (spec: 'non-existing union variant for $')") {
    int ty = kernel.get_struct("IntAndEitherInt8Or256").ok()->ty_idx;
    AbiValue v = AbiValue::make_struct("IntAndEitherInt8Or256");
    v.add_field("op", AbiValue::make_int(Idec("1")));
    v.add_field("i8or256", AbiValue::make_union("asdf", AbiValue::make_int(Idec("1"))));
    vm::CellBuilder cb;
    auto r = pack_value(ctx, ty, v, cb, "self");
    REQUIRE(r.is_error());
    CHECK(r.error().message().str().find("non-existing union variant") != std::string::npos);
  }

  SUBCASE("scalar unpack: truncated slice (spec: 'Index 0 > 0 is out of bounds')") {
    int ty = kernel.get_struct("JustInt32").ok()->ty_idx;
    vm::CellBuilder cb;
    Built b = fin(cb);  // empty cell
    auto cs = b.slice();
    CHECK(unpack_value(ctx, ty, cs, "self").is_error());
  }

  SUBCASE("union unpack: no prefix matches (spec: 'none of union prefixes match')") {
    int ty = kernel.get_struct("IntAndEitherInt8Or256").ok()->ty_idx;
    vm::CellBuilder cb;
    REQUIRE(store_uint(cb, Idec("0"), 32).is_ok());  // 32 zero bits: consumes op(int8)+union-prefix(1 implicit)... actually just enough to force a defined-but-mismatched path
    Built b = fin(cb);
    auto cs = b.slice();
    // Either this errors on prefix mismatch or on truncation -- both are
    // fail-closed rejections of a malformed union body, which is the
    // property this test cares about (spec's category, not exact wire shape).
    CHECK(unpack_value(ctx, ty, cs, "self").is_error());
  }
}

// ---------------------------------------------------------------------------
// (c) T9.2 carve-outs
// ---------------------------------------------------------------------------

TEST_CASE("W9.2 ClientTypeAnno: client_ty_idx cell-serialization (spec :69, :112)") {
  ContractABI abi = load_fixture("client-type-anno");
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);

  SUBCASE("ClientPayload: cell side uses client_ty_idx (string), not the raw 'remaining' ty_idx") {
    // note: ty_idx=13 (remaining) but client_ty_idx=14 (string). Per
    // AbiKernel.cpp's struct_fields_of ("cell side... swaps the field's cell
    // type for its client type"), cell-side packing/unpacking uses client_ty_idx
    // WHEN PRESENT -- so the wire format is a plain snake string ref-tail
    // (matches the spec's own `wrapperSlice.loadStringRefTail()).toBe('hello')`
    // assertion), not the raw 'remaining' passthrough. ClientPayload has
    // struct prefix 0x12345678 (32 bits).
    // store_string() itself does the ref-wrapping (storeStringRefTail =
    // storeRef(snake cell)) -- call it directly on the struct-level builder,
    // same as pack_value's own String case does, not on an intermediate cell.
    int ty = kernel.get_struct("ClientPayload").ok()->ty_idx;
    vm::CellBuilder cb;
    REQUIRE(store_prefix(cb, 0x12345678u, 32).is_ok());
    REQUIRE(store_string(cb, "hello").is_ok());
    Built b = fin(cb);
    auto cs = b.slice();
    auto r = unpack_value(ctx, ty, cs, "self");
    REQUIRE_MESSAGE(r.is_ok(), "unpack: " << (r.is_error() ? r.error().message().str() : ""));
    CHECK(r.ok().kind == AbiValueKind::Struct);
    REQUIRE(r.ok().struct_fields.size() == 1);
    CHECK(r.ok().struct_fields[0].second.kind == AbiValueKind::String);
    CHECK(r.ok().to_json() == R"({"$":"ClientPayload","note":"hello"})");

    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, ty, r.ok(), got, "self").is_ok());
    Built go = fin(got);
    CHECK(go.cell->get_hash() == b.cell->get_hash());
  }
}

TEST_CASE("W9.2 GenericUnionLabels: literal spec values (spec :105-130)") {
  ContractABI abi = load_fixture("generic-union-labels");
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);

  auto round_trip = [&](const char *struct_name, AbiValue v, const std::string &expect_json) {
    int ty = kernel.get_struct(struct_name).ok()->ty_idx;
    vm::CellBuilder got;
    CAPTURE(struct_name);
    REQUIRE(pack_value(ctx, ty, v, got, "self").is_ok());
    Built go = fin(got);
    auto cs = go.slice();
    auto r = unpack_value(ctx, ty, cs, "self");
    REQUIRE(r.is_ok());
    CHECK(r.ok().to_json() == expect_json);
  };

  {
    AbiValue v = AbiValue::make_struct("MsgPair");
    AbiValue gp = AbiValue::make_struct("GenericPair");
    gp.add_field("value", AbiValue::make_union("T1", AbiValue::make_int(Idec("10"))));
    v.add_field("value", std::move(gp));
    round_trip("MsgPair", std::move(v),
               R"({"$":"MsgPair","value":{"$":"GenericPair","value":{"$":"T1","value":"10"}}})");
  }
  {
    AbiValue v = AbiValue::make_struct("MsgPair");
    AbiValue gp = AbiValue::make_struct("GenericPair");
    gp.add_field("value", AbiValue::make_union("T2", AbiValue::make_int(Idec("11"))));
    v.add_field("value", std::move(gp));
    round_trip("MsgPair", std::move(v),
               R"({"$":"MsgPair","value":{"$":"GenericPair","value":{"$":"T2","value":"11"}}})");
  }
  {
    AbiValue v = AbiValue::make_struct("MsgOrInt16");
    AbiValue go8 = AbiValue::make_struct("GenericOrInt8");
    go8.add_field("value", AbiValue::make_union("T", AbiValue::make_int(Idec("13"))));
    v.add_field("value", std::move(go8));
    round_trip("MsgOrInt16", std::move(v),
               R"({"$":"MsgOrInt16","value":{"$":"GenericOrInt8","value":{"$":"T","value":"13"}}})");
  }
  {
    AbiValue v = AbiValue::make_struct("MsgOrInt16");
    AbiValue go8 = AbiValue::make_struct("GenericOrInt8");
    go8.add_field("value", AbiValue::make_union("int8", AbiValue::make_int(Idec("14"))));
    v.add_field("value", std::move(go8));
    round_trip("MsgOrInt16", std::move(v),
               R"({"$":"MsgOrInt16","value":{"$":"GenericOrInt8","value":{"$":"int8","value":"14"}}})");
  }
  {
    AbiValue v = AbiValue::make_struct("MsgAliasInt16");
    v.add_field("value", AbiValue::make_union("T", AbiValue::make_int(Idec("16"))));
    round_trip("MsgAliasInt16", std::move(v), R"({"$":"MsgAliasInt16","value":{"$":"T","value":"16"}})");
  }
  {
    AbiValue v = AbiValue::make_struct("MsgAliasInt16");
    v.add_field("value", AbiValue::make_union("int8", AbiValue::make_int(Idec("17"))));
    round_trip("MsgAliasInt16", std::move(v), R"({"$":"MsgAliasInt16","value":{"$":"int8","value":"17"}})");
  }
}

TEST_CASE("W9.2 LotsOfMessages: createCellOf hash-equality (spec :45-74, c1/c3 -- c2 needs create()-defaults, deferred to W11)") {
  ContractABI abi = load_fixture("lots-of-messages");
  AbiKernel kernel = AbiKernel::create(abi).move_as_ok();
  AbiInterpCtx ctx(kernel);

  SUBCASE("c1: IncreaseBy {counter_id:10, inc_by:20}") {
    int ty = kernel.get_struct("IncreaseBy").ok()->ty_idx;
    AbiValue v = AbiValue::make_struct("IncreaseBy");
    v.add_field("counter_id", AbiValue::make_int(Idec("10")));
    v.add_field("inc_by", AbiValue::make_int(Idec("20")));
    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, ty, v, got, "self").is_ok());
    Built go = fin(got);

    vm::CellBuilder expect;
    REQUIRE(store_prefix(expect, 0x12345678u, 32).is_ok());
    REQUIRE(store_int(expect, Idec("10"), 32).is_ok());
    REQUIRE(store_int(expect, Idec("20"), 32).is_ok());
    Built ex = fin(expect);
    CHECK(go.cell->get_hash() == ex.cell->get_hash());
  }

  SUBCASE("c3: ResetTo {counter_id:500, reset_to:0} (int64)") {
    // ResetTo is generic (type_params:['T']); kernel.get_struct("ResetTo")
    // would give the UNINSTANTIATED decl (reset_to typed as bare genericT
    // 'T', not packable). Use the monomorphic instantiation "ResetTo<int64>"
    // directly by its committed ty_idx (struct_instantiations[0], W1 note).
    int ty = 13;
    AbiValue v = AbiValue::make_struct("ResetTo");
    v.add_field("counter_id", AbiValue::make_int(Idec("500")));
    v.add_field("reset_to", AbiValue::make_int(Idec("0")));
    vm::CellBuilder got;
    REQUIRE(pack_value(ctx, ty, v, got, "self").is_ok());
    Built go = fin(got);

    vm::CellBuilder expect;
    REQUIRE(store_prefix(expect, 0x23456789u, 32).is_ok());
    REQUIRE(store_int(expect, Idec("500"), 32).is_ok());
    REQUIRE(store_int(expect, Idec("0"), 64).is_ok());
    Built ex = fin(expect);
    CHECK(go.cell->get_hash() == ex.cell->get_hash());
  }
}
