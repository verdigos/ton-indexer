// T0.3: data-driven conformance runner. Loads testdata/abi_vectors.json and
// exercises it one doctest SUBCASE per vector (doctest has no built-in
// mechanism for fully dynamic TEST_CASE registration by runtime string name;
// SUBCASE inside one TEST_CASE is the standard doctest idiom for data-driven
// suites and gives the same per-item pass/fail granularity in the report).
//
// Vectors carry `enabled_from` (a wave tag, e.g. "W8"). kEnabledWaves below
// is the set of wave tags THIS build actually has a driver for. At W0 it is
// empty on purpose (§4 TDD protocol: ABI-parse vectors enable at W1,
// value/hex vectors enable at W8 once the interpreter walker exists to drive
// them) -- every vector is skipped, not failed, so ctest is green at the
// wave boundary. Later waves add their tag here AND wire the actual
// parse/pack/compare logic in place of the FAIL() stub.

#include "doctest.h"

// td/utils/check.h (pulled in transitively below) redefines CHECK(...) as an
// abort-on-failure macro (::td::detail::process_check_error, [[noreturn]]),
// silently shadowing doctest's soft-check CHECK for the rest of this TU
// (observed as MSVC C4005 "macro redefinition"). Save/restore doctest's
// definition around the include so CHECK keeps doctest semantics here and in
// every wave that extends this file.
#pragma push_macro("CHECK")
#undef CHECK
#include "td/utils/JsonBuilder.h"
#pragma pop_macro("CHECK")

#include <fstream>
#include <set>
#include <sstream>
#include <string>

#ifndef TON_ABI_VECTORS_FILE
#error "TON_ABI_VECTORS_FILE must be defined by CMake to the abi_vectors.json path"
#endif

namespace {

using Json = td::JsonValue;
using JType = td::JsonValue::Type;

const std::set<std::string> kEnabledWaves = {};

const Json *jfield(const Json &e, td::Slice name) {
  if (e.type() != JType::Object) {
    return nullptr;
  }
  for (const auto &kv : e.get_object().field_values_) {
    if (kv.first == name) {
      return &kv.second;
    }
  }
  return nullptr;
}

std::string jstr(const Json &e, td::Slice name, const std::string &dflt = {}) {
  const Json *f = jfield(e, name);
  return (f != nullptr && f->type() == JType::String) ? f->get_string().str() : dflt;
}

// `buf` backs every Slice/string inside `root` (td::json_decode parses
// in-place) -- must outlive it, hence bundled together.
struct LoadedVectors {
  std::string buf;
  Json root;
};

LoadedVectors load_vectors_or_fail(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(static_cast<bool>(in), "cannot open vectors file: " << path);
  std::stringstream ss;
  ss << in.rdbuf();

  LoadedVectors lv;
  lv.buf = ss.str();
  auto r_root = td::json_decode(td::MutableSlice(lv.buf));
  REQUIRE_MESSAGE(r_root.is_ok(), "vectors JSON parse error in " << path);
  lv.root = r_root.move_as_ok();
  return lv;
}

}  // namespace

TEST_CASE("VectorRunner: data-driven ton-abi conformance") {
  LoadedVectors lv = load_vectors_or_fail(TON_ABI_VECTORS_FILE);
  REQUIRE(lv.root.type() == JType::Object);

  const Json *vectors = jfield(lv.root, "vectors");
  REQUIRE(vectors != nullptr);
  REQUIRE(vectors->type() == JType::Array);

  int enabled_count = 0;
  int skipped_count = 0;
  int idx = 0;

  for (const auto &v : vectors->get_array()) {
    std::string fixture = jstr(v, "fixture");
    std::string struct_name = jstr(v, "struct");
    std::string enabled_from = jstr(v, "enabled_from");
    std::string name = fixture + "::" + struct_name + "#" + std::to_string(idx);
    ++idx;

    if (kEnabledWaves.count(enabled_from) == 0) {
      // Not yet enabled for this build -- green-at-boundary, not a failure.
      ++skipped_count;
      continue;
    }

    SUBCASE(name.c_str()) {
      ++enabled_count;
      // W1+ (ABI-parse) / W8+ (value/hex pack-unpack) waves replace this
      // stub with the real driver: parse `value`, pack via AbiInterp, diff
      // against `golden_hex` (and the reverse for unpack).
      FAIL("vector enabled_from='" << enabled_from << "' but no driver wired yet: " << name);
    }
  }

  MESSAGE("ton-abi vectors: ", skipped_count, " skipped (not enabled), ", enabled_count, " run");
  CHECK(true);  // always at least one assertion so an all-skipped run reports as passed, not "no assertions"
}
