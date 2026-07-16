// T0.3/T8.3: data-driven conformance runner. Loads testdata/abi_vectors.json
// and exercises it one doctest SUBCASE per vector (doctest has no built-in
// mechanism for fully dynamic TEST_CASE registration by runtime string name;
// SUBCASE inside one TEST_CASE is the standard doctest idiom for data-driven
// suites and gives the same per-item pass/fail granularity in the report).
//
// Vectors carry `enabled_from` (a wave tag, e.g. "W8"). kEnabledWaves below
// is the set of wave tags THIS build actually has a driver for -- W8 lights
// up here (T8.3): the AbiInterp walker + AbiValue dump now exist to drive
// value/hex vectors both directions.
//
// Vector shapes (see testdata/abi_vectors.json):
//   normal:  {fixture, struct, value, golden_hex} -- unpack(golden_hex) must
//            canonical-JSON-equal `value`; pack(value-as-AbiValue) must
//            produce a cell whose HASH equals golden_hex's parsed cell (hex
//            string comparison isn't attempted -- see json_equal/parse_fift_hex
//            below for why hash equality is the right granularity here).
//   unpack_only: true -- skip the pack direction (e.g. the malformed-UTF8
//            string vector: lossy decode is not invertible byte-for-byte).
//   expect_error: true -- no `value`; unpack(golden_hex) must FAIL.

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

#include "AbiInterp.h"
#include "AbiKernel.h"
#include "AbiLoader.h"
#include "AbiValue.h"
#include "common/bitstring.h"
#include "common/refint.h"
#include "td/utils/base64.h"
#include "vm/boc.h"
#include "vm/cells/Cell.h"
#include "vm/cells/CellBuilder.h"
#include "vm/cells/CellSlice.h"
#pragma pop_macro("CHECK")

#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>

#ifndef TON_ABI_VECTORS_FILE
#error "TON_ABI_VECTORS_FILE must be defined by CMake to the abi_vectors.json path"
#endif
#ifndef TON_ABI_FIXTURES_DIR
#error "TON_ABI_FIXTURES_DIR must be defined by CMake"
#endif

namespace {

using namespace ton_abi;

using Json = td::JsonValue;
using JType = td::JsonValue::Type;

const std::set<std::string> kEnabledWaves = {"W8"};

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

bool jbool(const Json &e, td::Slice name, bool dflt = false) {
  const Json *f = jfield(e, name);
  return (f != nullptr && f->type() == JType::Boolean) ? f->get_boolean() : dflt;
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

// ---------------------------------------------------------------------------
// structural JSON equality (order-insensitive for objects, order-sensitive
// for arrays -- map entries/struct fields are already in canonical §0.14
// order on both sides, but comparing by key/index rather than raw text
// sidesteps incidental whitespace/quoting differences between td::JsonValue's
// own re-serialization and the file's formatting).
// ---------------------------------------------------------------------------

bool json_equal(const Json &a, const Json &b) {
  if (a.type() != b.type()) {
    return false;
  }
  switch (a.type()) {
    case JType::Null:
      return true;
    case JType::Boolean:
      return a.get_boolean() == b.get_boolean();
    case JType::Number:
      return a.get_number() == b.get_number();
    case JType::String:
      return a.get_string() == b.get_string();
    case JType::Array: {
      const auto &aa = a.get_array();
      const auto &bb = b.get_array();
      if (aa.size() != bb.size()) {
        return false;
      }
      for (std::size_t i = 0; i < aa.size(); ++i) {
        if (!json_equal(aa[i], bb[i])) {
          return false;
        }
      }
      return true;
    }
    case JType::Object: {
      const auto &ao = a.get_object().field_values_;
      const auto &bo = b.get_object();
      if (ao.size() != bo.field_values_.size()) {
        return false;
      }
      for (const auto &kv : ao) {
        const Json *bv = jfield(b, kv.first);
        if (bv == nullptr || !json_equal(kv.second, *bv)) {
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// fift-hex parser: "x{HEX}" (optionally with refs on following more-indented
// lines, one space per nesting level -- @ton/core's Cell.toString() format,
// see docs/ton-abi-plan.md VALUE_SCHEMA.md "Golden hex format"). Reuses the
// TON-native augmented-hex literal parser (td::bitstring::
// parse_bitstring_hex_literal, the exact inverse of the leaf's bits_to_hex),
// the same one ton-index-worker's own fift/vm tooling uses for "x{...}"
// literals (vm/utils.cpp:97-107).
// ---------------------------------------------------------------------------

struct FiftNode {
  std::string body;
  std::vector<FiftNode> children;
};

int leading_spaces(const std::string &s) {
  int n = 0;
  while (n < static_cast<int>(s.size()) && s[n] == ' ') {
    ++n;
  }
  return n;
}

std::size_t parse_fift_node(const std::vector<std::string> &lines, std::size_t idx, int depth, FiftNode &out) {
  out.body = lines[idx].substr(static_cast<std::size_t>(depth));
  std::size_t i = idx + 1;
  while (i < lines.size()) {
    int d = leading_spaces(lines[i]);
    if (d <= depth) {
      break;
    }
    FiftNode child;
    i = parse_fift_node(lines, i, d, child);
    out.children.push_back(std::move(child));
  }
  return i;
}

// Returns the finalized Ref<Cell> directly (not the CellBuilder -- CellBuilder
// is move-only and td::Result<T>'s internals want T at least copy-constructible).
td::Result<td::Ref<vm::Cell>> fift_node_cell(const FiftNode &node) {
  if (node.body.size() < 3 || node.body[0] != 'x' || node.body[1] != '{' || node.body.back() != '}') {
    return td::Status::Error(PSLICE() << "bad fift-hex node: '" << node.body << "'");
  }
  unsigned char buff[128];
  const std::string inner = node.body.substr(2, node.body.size() - 3);
  long bits = td::bitstring::parse_bitstring_hex_literal(buff, sizeof(buff), inner.data(), inner.data() + inner.size());
  if (bits < 0) {
    return td::Status::Error(PSLICE() << "failed to parse hex literal: '" << node.body << "'");
  }
  vm::CellBuilder cb;
  cb.store_bits(td::ConstBitPtr{buff}, static_cast<std::size_t>(bits));
  for (const auto &child : node.children) {
    TRY_RESULT(child_cell, fift_node_cell(child));
    if (!cb.store_ref_bool(child_cell)) {
      return td::Status::Error("store_ref failed while building fift-hex cell");
    }
  }
  return cb.finalize();
}

td::Result<td::Ref<vm::Cell>> parse_fift_hex(const std::string &text) {
  std::vector<std::string> lines;
  std::size_t pos = 0;
  while (true) {
    std::size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) {
      lines.push_back(text.substr(pos));
      break;
    }
    lines.push_back(text.substr(pos, nl - pos));
    pos = nl + 1;
  }
  if (lines.empty() || lines[0].empty()) {
    return td::Status::Error("empty fift-hex text");
  }
  FiftNode root;
  std::size_t consumed = parse_fift_node(lines, 0, 0, root);
  if (consumed != lines.size()) {
    return td::Status::Error("trailing garbage after root cell in fift-hex text");
  }
  return fift_node_cell(root);
}

// ---------------------------------------------------------------------------
// JSON -> AbiValue (the pack-direction inverse of AbiValue::to_json(), driven
// by the same ABI type tree unpack_value uses). Test-harness-only: production
// code never needs to construct an AbiValue from JSON, only dump one.
// ---------------------------------------------------------------------------

td::Result<AbiAddress> parse_address_string(const Json &j) {
  if (j.type() != JType::String) {
    return td::Status::Error("expected 'wc:hex' address string");
  }
  const std::string s = j.get_string().str();
  auto colon = s.find(':');
  if (colon == std::string::npos) {
    return td::Status::Error("malformed address string (missing ':')");
  }
  AbiAddress a;
  a.kind = AbiAddressKind::Std;
  a.workchain = std::atoi(s.substr(0, colon).c_str());
  std::string hex = s.substr(colon + 1);
  unsigned char buff[64];
  long bits = td::bitstring::parse_bitstring_hex_literal(buff, sizeof(buff), hex.data(), hex.data() + hex.size());
  if (bits != 256) {
    return td::Status::Error("address hash hex must decode to exactly 256 bits");
  }
  a.hash = td::Bits256(td::ConstBitPtr{buff});
  return a;
}

td::Result<AbiAddress> parse_address_extern_json(const Json &j) {
  const Json *ext = jfield(j, "extern");
  if (ext == nullptr || ext->type() != JType::Object) {
    return td::Status::Error("expected {\"extern\":{...}} object");
  }
  const Json *bitsF = jfield(*ext, "bits");
  const Json *valueF = jfield(*ext, "value");
  if (bitsF == nullptr || bitsF->type() != JType::Number || valueF == nullptr || valueF->type() != JType::String) {
    return td::Status::Error("malformed extern address object");
  }
  int ext_bits = std::atoi(bitsF->get_number().str().c_str());
  std::string hex = valueF->get_string().str();
  AbiAddress a;
  a.kind = AbiAddressKind::Extern;
  a.ext_bits = ext_bits;
  unsigned char buff[128];
  long bits = td::bitstring::parse_bitstring_hex_literal(buff, sizeof(buff), hex.data(), hex.data() + hex.size());
  if (bits < 0 || bits != ext_bits) {
    return td::Status::Error("extern address hex/bits mismatch");
  }
  vm::CellBuilder cb;
  cb.store_bits(td::ConstBitPtr{buff}, static_cast<std::size_t>(ext_bits));
  a.ext_value = vm::load_cell_slice_ref(cb.finalize());
  return a;
}

td::Result<AbiValue> abi_value_from_json(const AbiKernel &kernel, int ty_idx, const Json &j,
                                          std::optional<int> u_label_ty_idx) {
  TRY_RESULT(ty, kernel.ty_by_idx(ty_idx));

  switch (ty->kind) {
    case TyKind::IntN:
    case TyKind::UintN:
    case TyKind::VarIntN:
    case TyKind::VarUIntN:
    case TyKind::Coins: {
      if (j.type() != JType::String) {
        return td::Status::Error("expected decimal string for int-kind value");
      }
      return AbiValue::make_int(td::dec_string_to_int256(j.get_string().str()));
    }
    case TyKind::Bool: {
      if (j.type() != JType::Boolean) {
        return td::Status::Error("expected bool value");
      }
      return AbiValue::make_bool(j.get_boolean());
    }
    case TyKind::Cell: {
      if (j.type() != JType::String) {
        return td::Status::Error("expected base64 string for cell value");
      }
      TRY_RESULT(bytes, td::base64_decode(j.get_string()));
      TRY_RESULT(cell, vm::std_boc_deserialize(bytes));
      return AbiValue::make_cell(std::move(cell));
    }
    case TyKind::String: {
      if (j.type() != JType::String) {
        return td::Status::Error("expected string value");
      }
      return AbiValue::make_string(j.get_string().str());
    }
    case TyKind::Remaining:
    case TyKind::BitsN: {
      if (j.type() != JType::Object) {
        return td::Status::Error("expected {bits,refs} object");
      }
      const Json *bitsField = jfield(j, "bits");
      const Json *refsField = jfield(j, "refs");
      if (bitsField == nullptr || bitsField->type() != JType::String || refsField == nullptr ||
          refsField->type() != JType::Array) {
        return td::Status::Error("malformed {bits,refs} object");
      }
      vm::CellBuilder cb;
      for (char c : bitsField->get_string()) {
        if (!cb.store_long_bool(c == '1' ? 1 : 0, 1)) {
          return td::Status::Error("bit store failed while rebuilding bits/remaining value");
        }
      }
      for (const auto &r : refsField->get_array()) {
        if (r.type() != JType::String) {
          return td::Status::Error("ref must be a base64 string");
        }
        TRY_RESULT(bytes, td::base64_decode(r.get_string()));
        TRY_RESULT(refcell, vm::std_boc_deserialize(bytes));
        if (!cb.store_ref_bool(refcell)) {
          return td::Status::Error("store_ref failed while rebuilding bits/remaining value");
        }
      }
      return AbiValue::make_bits(vm::load_cell_slice_ref(cb.finalize()));
    }
    case TyKind::Address: {
      TRY_RESULT(a, parse_address_string(j));
      return AbiValue::make_address(std::move(a));
    }
    case TyKind::AddressOpt: {
      if (j.type() == JType::Null) {
        return AbiValue::make_null();
      }
      TRY_RESULT(a, parse_address_string(j));
      return AbiValue::make_address(std::move(a));
    }
    case TyKind::AddressExt: {
      TRY_RESULT(a, parse_address_extern_json(j));
      return AbiValue::make_address(std::move(a));
    }
    case TyKind::AddressAny: {
      if (j.type() == JType::String && j.get_string() == "none") {
        AbiAddress a;
        a.kind = AbiAddressKind::None;
        return AbiValue::make_address(a);
      }
      if (j.type() == JType::Object) {
        TRY_RESULT(a, parse_address_extern_json(j));
        return AbiValue::make_address(std::move(a));
      }
      TRY_RESULT(a, parse_address_string(j));
      return AbiValue::make_address(std::move(a));
    }
    case TyKind::NullLiteral:
      return AbiValue::make_null();
    case TyKind::Void:
      return AbiValue::make_void();
    case TyKind::Nullable: {
      if (j.type() == JType::Null) {
        return AbiValue::make_null();
      }
      return abi_value_from_json(kernel, std::get<TyNullable>(ty->data).inner_ty_idx, j, std::nullopt);
    }
    case TyKind::CellOf: {
      const Json *refField = jfield(j, "ref");
      if (refField == nullptr) {
        return td::Status::Error("expected {\"ref\":...} object for cellOf");
      }
      TRY_RESULT(inner, abi_value_from_json(kernel, std::get<TyInner>(ty->data).inner_ty_idx, *refField, std::nullopt));
      return AbiValue::make_cell_of(std::move(inner));
    }
    case TyKind::ArrayOf:
    case TyKind::LispListOf: {
      if (j.type() != JType::Array) {
        return td::Status::Error("expected array");
      }
      int inner_ty_idx = std::get<TyInner>(ty->data).inner_ty_idx;
      std::vector<AbiValue> items;
      for (const auto &e : j.get_array()) {
        TRY_RESULT(v, abi_value_from_json(kernel, inner_ty_idx, e, std::nullopt));
        items.push_back(std::move(v));
      }
      return AbiValue::make_list(std::move(items));
    }
    case TyKind::Tensor:
    case TyKind::ShapedTuple: {
      if (j.type() != JType::Array) {
        return td::Status::Error("expected array for tensor/shapedTuple");
      }
      const auto &items_ty_idx = std::get<TyItems>(ty->data).items_ty_idx;
      const auto &arr = j.get_array();
      if (arr.size() != items_ty_idx.size()) {
        return td::Status::Error("tensor/shapedTuple arity mismatch");
      }
      std::vector<AbiValue> items;
      for (std::size_t i = 0; i < items_ty_idx.size(); ++i) {
        TRY_RESULT(v, abi_value_from_json(kernel, items_ty_idx[i], arr[i], std::nullopt));
        items.push_back(std::move(v));
      }
      return AbiValue::make_list(std::move(items));
    }
    case TyKind::MapKV: {
      if (j.type() != JType::Array) {
        return td::Status::Error("expected array of [key,value] pairs");
      }
      const auto &m = std::get<TyMapKV>(ty->data);
      std::vector<std::pair<AbiValue, AbiValue>> entries;
      for (const auto &pairJ : j.get_array()) {
        if (pairJ.type() != JType::Array || pairJ.get_array().size() != 2) {
          return td::Status::Error("malformed map entry (expected [key,value])");
        }
        const auto &pa = pairJ.get_array();
        TRY_RESULT(k, abi_value_from_json(kernel, m.key_ty_idx, pa[0], std::nullopt));
        TRY_RESULT(v, abi_value_from_json(kernel, m.value_ty_idx, pa[1], std::nullopt));
        entries.emplace_back(std::move(k), std::move(v));
      }
      return AbiValue::make_map(std::move(entries));
    }
    case TyKind::EnumRef: {
      const auto &name = std::get<TyEnumRef>(ty->data).enum_name;
      TRY_RESULT(decl, kernel.get_enum(name));
      return abi_value_from_json(kernel, decl->encoded_as_ty_idx, j, std::nullopt);
    }
    case TyKind::StructRef: {
      const auto &name = std::get<TyStructRef>(ty->data).struct_name;
      TRY_RESULT(fields, kernel.struct_fields_of(ty_idx));
      AbiValue result = AbiValue::make_struct(name);
      for (const auto &f : *fields) {
        const Json *fv = jfield(j, f.name());
        if (fv == nullptr) {
          return td::Status::Error(PSLICE() << "missing field '" << f.name() << "' in JSON for struct '" << name
                                             << "'");
        }
        TRY_RESULT(v, abi_value_from_json(kernel, f.ty_idx, *fv, f.u_label_ty_idx));
        result.add_field(f.name(), std::move(v));
      }
      return result;
    }
    case TyKind::AliasRef: {
      TRY_RESULT(target, kernel.alias_target_of(ty_idx));
      return abi_value_from_json(kernel, target.ty_idx, j, target.u_label_ty_idx);
    }
    case TyKind::Union: {
      TRY_RESULT(ru, kernel.resolve_union(ty_idx, u_label_ty_idx));
      if (j.type() == JType::Object) {
        const Json *dollar = jfield(j, "$");
        if (dollar != nullptr && dollar->type() == JType::String && dollar->get_string() == "void") {
          return AbiValue::make_void();
        }
      }
      if (j.type() == JType::Null) {
        for (const auto &variant : ru.variants) {
          TRY_RESULT(vty, kernel.ty_by_idx(variant.variant_ty_idx));
          if (vty->kind == TyKind::NullLiteral) {
            return AbiValue::make_null();
          }
        }
        return td::Status::Error("union has no nullLiteral variant for a null JSON value");
      }
      if (j.type() != JType::Object) {
        return td::Status::Error("expected an object ($..) for a union value");
      }
      const Json *dollar = jfield(j, "$");
      if (dollar == nullptr || dollar->type() != JType::String) {
        return td::Status::Error("union value object missing '$'");
      }
      std::string label = dollar->get_string().str();
      for (const auto &variant : ru.variants) {
        if (variant.label_str != label) {
          continue;
        }
        if (variant.has_value_field) {
          const Json *valueField = jfield(j, "value");
          if (valueField == nullptr) {
            return td::Status::Error("union value object missing 'value'");
          }
          TRY_RESULT(inner, abi_value_from_json(kernel, variant.variant_ty_idx, *valueField, std::nullopt));
          return AbiValue::make_union(label, std::move(inner));
        }
        return abi_value_from_json(kernel, variant.variant_ty_idx, j, std::nullopt);
      }
      return td::Status::Error(PSLICE() << "no union variant matches $ = '" << label << "'");
    }
    default:
      return td::Status::Error("abi_value_from_json: unsupported ty kind");
  }
}

// ---------------------------------------------------------------------------
// fixture context cache: one (ContractABI, AbiKernel, AbiInterpCtx) bundle
// per fixture name, constructed IN PLACE inside the map node (try_emplace)
// and never moved afterward -- AbiKernel holds a non-owning pointer into its
// ContractABI, so the bundle's address must stay fixed for its lifetime.
// ---------------------------------------------------------------------------

struct FixtureCtx {
  ContractABI abi;
  AbiKernel kernel;
  AbiInterpCtx ctx;

  explicit FixtureCtx(ContractABI abi_in)
      : abi(std::move(abi_in)), kernel(AbiKernel::create(abi).move_as_ok()), ctx(kernel) {
  }

  FixtureCtx(const FixtureCtx &) = delete;
  FixtureCtx(FixtureCtx &&) = delete;
};

std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(static_cast<bool>(in), "cannot open fixture: " << path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

FixtureCtx &fixture_ctx_for(std::unordered_map<std::string, std::unique_ptr<FixtureCtx>> &cache,
                            const std::string &fixture) {
  auto it = cache.find(fixture);
  if (it != cache.end()) {
    return *it->second;
  }
  std::string path = std::string(TON_ABI_FIXTURES_DIR) + "/" + fixture + ".abi.json";
  auto r = load_abi_from_json(read_file(path));
  REQUIRE_MESSAGE(r.is_ok(), fixture << ": " << (r.is_error() ? r.error().message().str() : ""));
  auto inserted = cache.emplace(fixture, std::make_unique<FixtureCtx>(r.move_as_ok()));
  return *inserted.first->second;
}

}  // namespace

TEST_CASE("VectorRunner: data-driven ton-abi conformance") {
  LoadedVectors lv = load_vectors_or_fail(TON_ABI_VECTORS_FILE);
  REQUIRE(lv.root.type() == JType::Object);

  const Json *vectors = jfield(lv.root, "vectors");
  REQUIRE(vectors != nullptr);
  REQUIRE(vectors->type() == JType::Array);

  std::unordered_map<std::string, std::unique_ptr<FixtureCtx>> fixtures;

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

      FixtureCtx &fc = fixture_ctx_for(fixtures, fixture);
      auto r_struct = fc.kernel.get_struct(struct_name);
      REQUIRE_MESSAGE(r_struct.is_ok(), name << ": " << r_struct.error().message().str());
      int struct_ty_idx = r_struct.ok()->ty_idx;

      std::string golden_hex = jstr(v, "golden_hex");
      auto r_cell = parse_fift_hex(golden_hex);
      REQUIRE_MESSAGE(r_cell.is_ok(), name << ": fift-hex parse: " << r_cell.error().message().str());
      td::Ref<vm::Cell> golden_cell = r_cell.move_as_ok();

      if (jbool(v, "expect_error")) {
        auto cs = vm::load_cell_slice(golden_cell);
        auto r_unpack = unpack_value(fc.ctx, struct_ty_idx, cs, "self");
        CHECK_MESSAGE(r_unpack.is_error(), name << ": expected unpack to fail but it succeeded");
        continue;
      }

      const Json *expected_value = jfield(v, "value");
      REQUIRE_MESSAGE(expected_value != nullptr, name << ": vector missing 'value'");

      // --- unpack direction: golden_hex -> AbiValue -> canonical JSON ---
      auto cs = vm::load_cell_slice(golden_cell);
      auto r_unpack = unpack_value(fc.ctx, struct_ty_idx, cs, "self");
      REQUIRE_MESSAGE(r_unpack.is_ok(), name << ": unpack: " << r_unpack.error().message().str());
      std::string dumped = r_unpack.ok().to_json();

      auto r_dumped_json = td::json_decode(td::MutableSlice(dumped));
      REQUIRE_MESSAGE(r_dumped_json.is_ok(), name << ": re-parsing our own dump: " << r_dumped_json.error().message().str());
      Json dumped_json = r_dumped_json.move_as_ok();
      CHECK_MESSAGE(json_equal(dumped_json, *expected_value),
                    name << ": unpacked value-JSON mismatch\n  got:  " << dumped << "\n  fixture expects the vector's 'value'");

      if (jbool(v, "unpack_only")) {
        continue;
      }

      // --- pack direction: value-JSON -> AbiValue -> cell hash == golden ---
      auto r_value = abi_value_from_json(fc.kernel, struct_ty_idx, *expected_value, std::nullopt);
      REQUIRE_MESSAGE(r_value.is_ok(), name << ": value-JSON -> AbiValue: " << r_value.error().message().str());

      vm::CellBuilder cb;
      auto r_pack = pack_value(fc.ctx, struct_ty_idx, r_value.ok(), cb, "self");
      REQUIRE_MESSAGE(r_pack.is_ok(), name << ": pack: " << r_pack.error().message().str());
      td::Ref<vm::Cell> packed_cell = cb.finalize();
      CHECK_MESSAGE(packed_cell->get_hash() == golden_cell->get_hash(), name << ": packed cell hash != golden hex's cell hash");
    }
  }

  MESSAGE("ton-abi vectors: ", skipped_count, " skipped (not enabled), ", enabled_count, " run");
  CHECK(true);  // always at least one assertion so an all-skipped run reports as passed, not "no assertions"
}
