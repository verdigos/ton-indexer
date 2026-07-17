// W11 T11.1: ton-abi-gen emitter units.
//  (A) The emitted output for tolk_counter / lots-of-messages / generic-union-
//      labels COMPILES (this TU includes the generated headers and links their
//      committed .cpp) and basic-round-trips: for hand-built cells, the
//      generated struct's from_slice -> to_abi_value -> to_json equals the
//      interpreter's dump (a T11.1 slice of the T11.2 A/B gate), and the
//      generated store round-trips to the same cell hash.
//  (B) Per-direction custom gate (shape spec §6, plan §0.19 rider): a ONE-SIDED
//      custom STRUCT delegates ONLY the registered direction to the typed
//      registry; the other direction keeps the normal baked path. This is the
//      reference-emitter bug (unpack gated on the pack flag) that we must NOT
//      port -- proven at codegen level since the A/B gate cannot catch it.
//  (C) Decl-level non-standard map key -> HARD generation failure (§7).
//  (D) Non-serializable field (callable) -> compiling stubs, both directions.

#include "doctest.h"

#pragma push_macro("CHECK")
#undef CHECK
#include "AbiEmit.h"
#include "AbiKernel.h"
#include "AbiLeavesPrefix.h"
#include "AbiLeavesScalar.h"
#include "AbiLoader.h"
#include "AbiValue.h"

#include "common/refint.h"
#include "vm/cells/Cell.h"
#include "vm/cells/CellBuilder.h"
#include "vm/cells/CellSlice.h"

#include "tolk_counter_gen.h"
#include "lots_of_messages_gen.h"
#include "generic_union_labels_gen.h"
#pragma pop_macro("CHECK")

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
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

std::string emit_source_of(const std::string &fixture) {
  std::string path = std::string(TON_ABI_FIXTURES_DIR) + "/" + fixture + ".abi.json";
  auto abi = load_abi_from_json(read_file(path)).move_as_ok();
  auto box = std::make_unique<ContractABI>(std::move(abi));
  auto kernel = AbiKernel::create(*box).move_as_ok();
  auto r = emit_abi(*box, kernel);
  REQUIRE_MESSAGE(r.is_ok(), fixture << " emit: " << (r.is_error() ? r.error().message().str() : ""));
  return r.move_as_ok().source;
}

}  // namespace

// ---------------------------------------------------------------------------
// (A) generated vs interpreter, tolk_counter scalars + prefix
// ---------------------------------------------------------------------------
TEST_CASE("emit: tolk_counter IncreaseCounter generated dump + store round-trip") {
  vm::CellBuilder cb;
  REQUIRE(store_prefix(cb, 0x7E8764EFULL, 32).is_ok());
  REQUIRE(store_uint(cb, td::make_refint(123456789), 64).is_ok());
  REQUIRE(store_uint(cb, td::make_refint(4294967295LL), 32).is_ok());
  td::Ref<vm::Cell> cell = cb.finalize();

  auto cs = vm::load_cell_slice(cell);
  auto g = gen::tolk_counter::IncreaseCounter::from_slice(cs);
  REQUIRE_MESSAGE(g.is_ok(), "gen from_slice: " << (g.is_error() ? g.error().message().str() : ""));
  auto gv = g.ok().to_abi_value();
  REQUIRE(gv.is_ok());
  std::string gen_dump = gv.ok().to_json();

  CHECK(gen_dump == R"({"$":"IncreaseCounter","queryId":"123456789","increaseBy":"4294967295"})");

  // store round-trip: from_slice value re-stored == original cell hash.
  vm::CellBuilder cb2;
  REQUIRE(g.ok().store(cb2).is_ok());
  CHECK(cb2.finalize()->get_hash() == cell->get_hash());
}

TEST_CASE("emit: tolk_counter IncreaseCounter prefix mismatch fails") {
  vm::CellBuilder cb;
  REQUIRE(store_prefix(cb, 0x7E8764E0ULL, 32).is_ok());  // flipped nibble
  REQUIRE(store_uint(cb, td::make_refint(0), 64).is_ok());
  REQUIRE(store_uint(cb, td::make_refint(0), 32).is_ok());
  auto cs = vm::load_cell_slice(cb.finalize());
  CHECK(gen::tolk_counter::IncreaseCounter::from_slice(cs).is_error());
}

// ---------------------------------------------------------------------------
// (A) generated vs interpreter, lots-of-messages: monomorph + empty struct
// ---------------------------------------------------------------------------
TEST_CASE("emit: lots-of-messages IncreaseBy negative int + default field") {
  vm::CellBuilder cb;
  REQUIRE(store_prefix(cb, 0x12345678ULL, 32).is_ok());
  REQUIRE(store_int(cb, td::make_refint(-5), 32).is_ok());
  REQUIRE(store_int(cb, td::make_refint(100), 32).is_ok());
  td::Ref<vm::Cell> cell = cb.finalize();

  auto cs = vm::load_cell_slice(cell);
  auto g = gen::lots_of_messages::IncreaseBy::from_slice(cs);
  REQUIRE(g.is_ok());
  std::string gen_dump = g.ok().to_abi_value().move_as_ok().to_json();
  CHECK(gen_dump == R"({"$":"IncreaseBy","counter_id":"-5","inc_by":"100"})");
}

TEST_CASE("emit: lots-of-messages EmptyMsg + create()-with-default") {
  vm::CellBuilder cb;
  REQUIRE(store_prefix(cb, 0x34567890ULL, 32).is_ok());
  td::Ref<vm::Cell> cell = cb.finalize();
  auto cs = vm::load_cell_slice(cell);
  auto g = gen::lots_of_messages::EmptyMsg::from_slice(cs);
  REQUIRE(g.is_ok());
  CHECK(g.ok().to_abi_value().move_as_ok().to_json() == R"({"$":"EmptyMsg"})");

  // create() materializes the inc_by=1 default when the optional arg is absent.
  gen::lots_of_messages::IncreaseBy::CreateArgs args;
  args.counter_id = 7;  // int8 -> td::int64 (W11.5)
  auto made = gen::lots_of_messages::IncreaseBy::create(args);
  CHECK(made.inc_by == 1);       // int32 -> td::int64
  CHECK(made.counter_id == 7);
}

// ---------------------------------------------------------------------------
// (A) generated vs interpreter, generic-union-labels: struct->generic->union
// ---------------------------------------------------------------------------
TEST_CASE("emit: generic-union-labels MsgPair T1 union label round-trip") {
  // MsgPair(0x10000001) -> GenericPair.value -> union{T1=int32}: implicit bit 0
  // then int32(5).
  vm::CellBuilder cb;
  REQUIRE(store_prefix(cb, 0x10000001ULL, 32).is_ok());
  REQUIRE(cb.store_long_bool(0, 1));  // union implicit prefix, variant 0 (T1)
  REQUIRE(store_int(cb, td::make_refint(5), 32).is_ok());
  td::Ref<vm::Cell> cell = cb.finalize();

  auto cs = vm::load_cell_slice(cell);
  auto g = gen::generic_union_labels::MsgPair::from_slice(cs);
  REQUIRE_MESSAGE(g.is_ok(), "gen from_slice: " << (g.is_error() ? g.error().message().str() : ""));
  std::string gen_dump = g.ok().to_abi_value().move_as_ok().to_json();

  CHECK(gen_dump ==
        R"({"$":"MsgPair","value":{"$":"GenericPair","value":{"$":"T1","value":"5"}}})");

  vm::CellBuilder cb2;
  REQUIRE(g.ok().store(cb2).is_ok());
  CHECK(cb2.finalize()->get_hash() == cell->get_hash());
}

TEST_CASE("emit: generic-union-labels MsgAliasInt16 T union label round-trip") {
  // MsgAliasInt16(0x10000005) -> AliasOrInt8_int16 union{T=int16}: implicit bit 0
  // then int16(5).
  vm::CellBuilder cb;
  REQUIRE(store_prefix(cb, 0x10000005ULL, 32).is_ok());
  REQUIRE(cb.store_long_bool(0, 1));
  REQUIRE(store_int(cb, td::make_refint(5), 16).is_ok());
  td::Ref<vm::Cell> cell = cb.finalize();
  auto cs = vm::load_cell_slice(cell);
  auto g = gen::generic_union_labels::MsgAliasInt16::from_slice(cs);
  REQUIRE(g.is_ok());
  CHECK(g.ok().to_abi_value().move_as_ok().to_json() ==
        R"({"$":"MsgAliasInt16","value":{"$":"T","value":"5"}})");
}

// ---------------------------------------------------------------------------
// (B) per-direction custom gate (plan §0.19 -- do NOT port the reference bug)
// ---------------------------------------------------------------------------
namespace {
// Minimal hand-authored ABI: struct S with a single uint8 field `a`, carrying a
// custom_pack_unpack with the two booleans templated in.
std::string one_sided_custom_json(bool pack, bool unpack) {
  std::string p = pack ? "true" : "false";
  std::string u = unpack ? "true" : "false";
  return R"({
    "abi_schema_version": "1.0",
    "contract_name": "C",
    "unique_types": [ {"kind":"uintN","n":8}, {"kind":"StructRef","struct_name":"S"} ],
    "struct_instantiations": [], "alias_instantiations": [],
    "declarations": [
      { "kind":"struct", "name":"S", "ty_idx":1,
        "custom_pack_unpack": { "pack_to_builder": )" +
         p + R"(, "unpack_from_slice": )" + u + R"( },
        "fields": [ {"name":"a","ty_idx":0} ] }
    ],
    "storage": {}, "incoming_messages": [], "incoming_external": [],
    "outgoing_messages": [], "emitted_events": [], "get_methods": [], "thrown_errors": [],
    "compiler_name":"hand", "compiler_version":"n/a"
  })";
}

std::string emit_json_source(const std::string &json) {
  auto abi = load_abi_from_json(json).move_as_ok();
  auto box = std::make_unique<ContractABI>(std::move(abi));
  auto kernel = AbiKernel::create(*box).move_as_ok();
  return emit_abi(*box, kernel).move_as_ok().source;
}

// crude "does the body of method `sig` mention `needle`" check.
bool body_mentions(const std::string &src, const std::string &sig, const std::string &needle) {
  auto start = src.find(sig);
  REQUIRE(start != std::string::npos);
  auto next = src.find("\n}\n", start);
  std::string body = src.substr(start, next == std::string::npos ? std::string::npos : next - start);
  return body.find(needle) != std::string::npos;
}
}  // namespace

TEST_CASE("emit: pack-only custom struct delegates ONLY store, not from_slice") {
  std::string src = emit_json_source(one_sided_custom_json(/*pack*/ true, /*unpack*/ false));
  CHECK(body_mentions(src, "S::store(", "abi_custom_lookup"));         // pack -> registry
  CHECK_FALSE(body_mentions(src, "S::from_slice(", "abi_custom_lookup"));  // unpack -> normal
  CHECK(body_mentions(src, "S::from_slice(", "load_uint"));            // normal baked path
}

TEST_CASE("emit: unpack-only custom struct delegates ONLY from_slice, not store") {
  std::string src = emit_json_source(one_sided_custom_json(/*pack*/ false, /*unpack*/ true));
  CHECK(body_mentions(src, "S::from_slice(", "abi_custom_lookup"));    // unpack -> registry
  CHECK_FALSE(body_mentions(src, "S::store(", "abi_custom_lookup"));   // pack -> normal
  CHECK(body_mentions(src, "S::store(", "store_uint"));               // normal baked path
}

// ---------------------------------------------------------------------------
// (C) decl-level non-standard map key -> HARD failure (ErrorsAtCodegen parity)
// ---------------------------------------------------------------------------
TEST_CASE("emit: err-invalid-map-key hard-fails with map-key error") {
  for (const char *fx : {"err-invalid-map-key-1", "err-invalid-map-key-2"}) {
    std::string path = std::string(TON_ABI_FIXTURES_DIR) + "/" + fx + ".abi.json";
    auto abi = load_abi_from_json(read_file(path)).move_as_ok();
    auto box = std::make_unique<ContractABI>(std::move(abi));
    auto kernel = AbiKernel::create(*box).move_as_ok();
    auto r = emit_abi(*box, kernel);
    REQUIRE_MESSAGE(r.is_error(), fx << " expected hard failure");
    CHECK(r.error().message().str().find("map-key") != std::string::npos);
  }
}

// ---------------------------------------------------------------------------
// (D) non-serializable field (callable) -> compiling stubs (§0.18)
// ---------------------------------------------------------------------------
TEST_CASE("emit: err-cont-on-stack emits cleanly; callable field -> stub") {
  // -1 has `struct D { n: callable }` -> the callable member is omitted and both
  // directions become runtime-error stubs (the file still emits cleanly, no
  // hard fail -- §0.18: err-cont ABIs must NOT fail).
  std::string src1 = emit_source_of("err-cont-on-stack-1");
  CHECK(src1.find("not serializable") != std::string::npos);
  CHECK(src1.find("D::from_slice") != std::string::npos);

  // -2 carries `callable` only in a get-method (out of scope): no struct field
  // is non-serializable, so it emits cleanly with no stub.
  std::string src2 = emit_source_of("err-cont-on-stack-2");
  CHECK(src2.find("not serializable") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Cli-analog (shape spec §11 / plan T11.3): the ton-abi-gen EXE writes a pair
// to disk; the emitted header contains the struct's signature.
// ---------------------------------------------------------------------------
#if defined(TON_ABI_GEN_EXE) && defined(TON_ABI_TEST_TMP)
TEST_CASE("cli: ton-abi-gen writes a header containing the struct signature") {
  namespace fs = std::filesystem;
  fs::path out = TON_ABI_TEST_TMP;
  std::error_code ec;
  fs::create_directories(out, ec);

  std::string abi = std::string(TON_ABI_FIXTURES_DIR) + "/tolk_counter.abi.json";
  std::string cmd = "\"\"" + std::string(TON_ABI_GEN_EXE) + "\" \"" + abi + "\" --out-dir \"" + out.string() + "\"\"";
  int rc = std::system(cmd.c_str());
  REQUIRE_MESSAGE(rc == 0, "ton-abi-gen exited " << rc << " for: " << cmd);

  std::string header = read_file((out / "tolk_counter_gen.h").string());
  CHECK(header.find("struct IncreaseCounter {") != std::string::npos);
  CHECK(header.find("static td::Result<IncreaseCounter> from_slice(vm::CellSlice& cs);") != std::string::npos);
}
#endif
