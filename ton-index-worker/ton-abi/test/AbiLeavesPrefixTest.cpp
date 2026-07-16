// W7 TDD: prefix ops + union prefix-tree dispatch. Prefix checks over hand-built
// cells; dispatch over ResolvedUnions built from hand-built union ABIs (via the
// W2 kernel). Cases: implicit-eat, explicit-re-read (peek only), T|void empty,
// Either both branches, no-match reject, trailing-garbage-after-void reject.

#include "doctest.h"

#pragma push_macro("CHECK")
#undef CHECK
#include "AbiLeavesPrefix.h"
#include "AbiKernel.h"
#include "AbiLoader.h"
#include "vm/cells/CellSlice.h"
#pragma pop_macro("CHECK")

#include <string>

namespace {

using namespace ton_abi;

vm::CellSlice cs_of(vm::CellBuilder &cb) { return vm::load_cell_slice(cb.finalize()); }

ContractABI union_abi(const std::string &unique_types) {
  std::string json =
      std::string(R"({"contract_name":"X","abi_schema_version":"1.0","unique_types":)") + unique_types +
      R"(,"struct_instantiations":[],"alias_instantiations":[],"declarations":[],"storage":{},)"
      R"("incoming_messages":[],"incoming_external":[],"outgoing_messages":[],"emitted_events":[],)"
      R"("get_methods":[],"thrown_errors":[],"compiler_name":"tolk","compiler_version":"1.4.1"})";
  auto r = load_abi_from_json(json);
  REQUIRE_MESSAGE(r.is_ok(), (r.is_error() ? r.error().message().str() : ""));
  return r.move_as_ok();
}

ResolvedUnion resolve(const AbiKernel &k, int ty_idx) {
  auto r = k.resolve_union(ty_idx, std::nullopt);
  REQUIRE(r.is_ok());
  return r.move_as_ok();
}

}  // namespace

// ---------------------------------------------------------------------------
// prefix ops
// ---------------------------------------------------------------------------

TEST_CASE("prefix: store + check round-trip, mismatch rejects") {
  vm::CellBuilder cb;
  REQUIRE(store_prefix(cb, 0x12345678u, 32).is_ok());
  auto ok = cs_of(cb);
  CHECK(load_and_check_prefix(ok, 0x12345678u, 32).is_ok());

  auto bad = cs_of(cb);
  CHECK(load_and_check_prefix(bad, 0x12345679u, 32).is_error());
}

TEST_CASE("prefix: 48-bit struct opcode round-trip (wider than the 32-bit union bound)") {
  vm::CellBuilder cb;
  REQUIRE(store_prefix(cb, 0x123456789ABCull, 48).is_ok());
  auto cs = cs_of(cb);
  CHECK(cs.size() == 48);
  CHECK(load_and_check_prefix(cs, 0x123456789ABCull, 48).is_ok());
}

TEST_CASE("prefix: length 0 always matches and consumes nothing") {
  vm::CellBuilder cb;
  REQUIRE(cb.store_long_bool(0xF, 4));
  auto cs = cs_of(cb);
  CHECK(load_and_check_prefix(cs, 0, 0).is_ok());
  CHECK(cs.size() == 4);  // untouched
}

TEST_CASE("prefix: check truncation rejects") {
  vm::CellBuilder cb;
  REQUIRE(cb.store_long_bool(0x1, 4));  // only 4 bits
  auto cs = cs_of(cb);
  CHECK(load_and_check_prefix(cs, 0, 8).is_error());
}

TEST_CASE("prefix: lookup peeks without consuming") {
  vm::CellBuilder cb;
  REQUIRE(cb.store_long_bool(0xAB, 8));
  auto cs = cs_of(cb);
  CHECK(lookup_prefix(cs, 0xAB, 8));
  CHECK_FALSE(lookup_prefix(cs, 0xAC, 8));
  CHECK(cs.size() == 8);  // not consumed
}

// ---------------------------------------------------------------------------
// union dispatch
// ---------------------------------------------------------------------------

TEST_CASE("union dispatch: Either (2-variant implicit) eats the 1-bit prefix") {
  // int8 | int16, implicit prefixes 0b0 / 0b1.
  auto abi = union_abi(R"([{"kind":"intN","n":8},{"kind":"intN","n":16},
    {"kind":"union","variants":[
      {"variant_ty_idx":0,"prefix_num":0,"prefix_len":1,"is_prefix_implicit":true},
      {"variant_ty_idx":1,"prefix_num":1,"prefix_len":1,"is_prefix_implicit":true}]}])");
  auto k = AbiKernel::create(abi).move_as_ok();
  auto ru = resolve(k, 2);

  SUBCASE("bit 0 -> variant 0") {
    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(0, 1));
    REQUIRE(cb.store_long_bool(0x55, 8));
    auto cs = cs_of(cb);
    auto d = dispatch_union(cs, ru);
    REQUIRE(d.is_ok());
    CHECK(d.ok().variant_index == 0);
    CHECK_FALSE(d.ok().is_void);
    CHECK(cs.size() == 8);  // 1-bit implicit prefix eaten
  }
  SUBCASE("bit 1 -> variant 1") {
    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(1, 1));
    REQUIRE(cb.store_long_bool(0x1234, 16));
    auto cs = cs_of(cb);
    auto d = dispatch_union(cs, ru);
    REQUIRE(d.is_ok());
    CHECK(d.ok().variant_index == 1);
    CHECK(cs.size() == 16);  // implicit prefix eaten
  }
}

TEST_CASE("union dispatch: explicit prefix is peeked, NOT consumed (variant re-reads)") {
  // Two explicit 8-bit prefixes 0xA / 0xB.
  auto abi = union_abi(R"([{"kind":"intN","n":8},
    {"kind":"union","variants":[
      {"variant_ty_idx":0,"prefix_num":10,"prefix_len":8,"is_prefix_implicit":false},
      {"variant_ty_idx":0,"prefix_num":11,"prefix_len":8,"is_prefix_implicit":false}]}])");
  auto k = AbiKernel::create(abi).move_as_ok();
  auto ru = resolve(k, 1);

  vm::CellBuilder cb;
  REQUIRE(cb.store_long_bool(10, 8));   // matches variant 0's explicit prefix
  REQUIRE(cb.store_long_bool(0x55, 8));
  auto cs = cs_of(cb);
  auto d = dispatch_union(cs, ru);
  REQUIRE(d.is_ok());
  CHECK(d.ok().variant_index == 0);
  CHECK(cs.size() == 16);  // NOTHING consumed -- explicit prefix left for re-read
}

TEST_CASE("union dispatch: T|void -> empty slice is void, non-empty is T") {
  auto abi = union_abi(R"([{"kind":"intN","n":8},{"kind":"void"},
    {"kind":"union","variants":[
      {"variant_ty_idx":0,"prefix_num":0,"prefix_len":1,"is_prefix_implicit":true},
      {"variant_ty_idx":1,"prefix_num":0,"prefix_len":0}]}])");
  auto k = AbiKernel::create(abi).move_as_ok();
  auto ru = resolve(k, 2);
  REQUIRE(ru.has_void);

  SUBCASE("empty -> void") {
    vm::CellBuilder cb;
    auto cs = cs_of(cb);
    auto d = dispatch_union(cs, ru);
    REQUIRE(d.is_ok());
    CHECK(d.ok().is_void);
    CHECK(d.ok().variant_index == 1);  // the trailing void
  }
  SUBCASE("non-empty -> T, implicit prefix eaten") {
    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(0, 1));
    REQUIRE(cb.store_long_bool(0x55, 8));
    auto cs = cs_of(cb);
    auto d = dispatch_union(cs, ru);
    REQUIRE(d.is_ok());
    CHECK_FALSE(d.ok().is_void);
    CHECK(d.ok().variant_index == 0);
    CHECK(cs.size() == 8);
  }
}

TEST_CASE("union dispatch: no matching prefix and no void -> error") {
  auto abi = union_abi(R"([{"kind":"intN","n":8},
    {"kind":"union","variants":[
      {"variant_ty_idx":0,"prefix_num":10,"prefix_len":8,"is_prefix_implicit":false},
      {"variant_ty_idx":0,"prefix_num":11,"prefix_len":8,"is_prefix_implicit":false}]}])");
  auto k = AbiKernel::create(abi).move_as_ok();
  auto ru = resolve(k, 1);
  vm::CellBuilder cb;
  REQUIRE(cb.store_long_bool(0xC, 8));  // neither 0xA nor 0xB
  auto cs = cs_of(cb);
  CHECK(dispatch_union(cs, ru).is_error());
}

TEST_CASE("union dispatch: general-with-void, non-empty no-match rejects (not void)") {
  auto abi = union_abi(R"([{"kind":"intN","n":8},{"kind":"void"},
    {"kind":"union","variants":[
      {"variant_ty_idx":0,"prefix_num":10,"prefix_len":8,"is_prefix_implicit":false},
      {"variant_ty_idx":0,"prefix_num":11,"prefix_len":8,"is_prefix_implicit":false},
      {"variant_ty_idx":1,"prefix_num":0,"prefix_len":0}]}])");
  auto k = AbiKernel::create(abi).move_as_ok();
  auto ru = resolve(k, 2);
  REQUIRE(ru.has_void);

  SUBCASE("non-empty garbage -> error") {
    vm::CellBuilder cb;
    REQUIRE(cb.store_long_bool(0xC, 8));
    auto cs = cs_of(cb);
    CHECK(dispatch_union(cs, ru).is_error());
  }
  SUBCASE("empty -> void (matched last, after prefix probes fail)") {
    vm::CellBuilder cb;
    auto cs = cs_of(cb);
    auto d = dispatch_union(cs, ru);
    REQUIRE(d.is_ok());
    CHECK(d.ok().is_void);
    CHECK(d.ok().variant_index == 2);
  }
}

// ---------------------------------------------------------------------------
// store-side prefix policy
// ---------------------------------------------------------------------------

TEST_CASE("store_union_prefix: implicit writes the prefix, explicit writes nothing") {
  // Implicit variant.
  LabeledVariant impl;
  impl.prefix_num = 1;
  impl.prefix_len = 1;
  impl.is_prefix_implicit = true;
  vm::CellBuilder a;
  REQUIRE(store_union_prefix(a, impl).is_ok());
  CHECK(cs_of(a).size() == 1);

  // Explicit variant: the union writes nothing (variant's own store does).
  LabeledVariant expl;
  expl.prefix_num = 0xAB;
  expl.prefix_len = 8;
  expl.is_prefix_implicit = false;
  vm::CellBuilder b;
  REQUIRE(store_union_prefix(b, expl).is_ok());
  CHECK(cs_of(b).size() == 0);
}
