#include "VectorRunner.h"

#include "ExprEval.h"
#include "GenVectors.h"
#include "Value.h"

#include "common/refint.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/base64.h"
#include "vm/boc.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace mch {

namespace {

using Json = td::JsonValue;
using JType = td::JsonValue::Type;

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

bool has_tag(const Json &e, td::Slice tag) {
  return e.type() == JType::Object && jfield(e, tag) != nullptr;
}

Value decode(const Json &e);

Value::Fields decode_fields(const Json &obj) {
  Value::Fields fs;
  for (const auto &kv : obj.get_object().field_values_) {
    fs.emplace_back(kv.first.str(), decode(kv.second));
  }
  return fs;
}

// §12 value-encoding convention -> Value.
Value decode(const Json &e) {
  switch (e.type()) {
    case JType::Null:
      return Value::null();
    case JType::Boolean:
      return Value::make_bool(e.get_boolean());
    case JType::Number:
      return Value::make_int(td::dec_string_to_int256(e.get_number().str()));
    case JType::String:
      return Value::make_str(e.get_string().str());
    case JType::Array: {
      std::vector<Value> xs;
      for (const auto &it : e.get_array()) {
        xs.push_back(decode(it));
      }
      return Value::make_list(std::move(xs));
    }
    case JType::Object:
      break;
  }

  if (has_tag(e, "$amount")) {
    return Value::make_amount(td::dec_string_to_int256(jfield(e, "$amount")->get_number().str()));
  }
  if (has_tag(e, "$account")) {
    const Json *a = jfield(e, "$account");
    if (a->type() == JType::Null) {
      return Value::make_account_none();
    }
    const std::string raw = a->get_string().str();
    auto norm = normalize_raw_address(raw);
    return Value::make_account_raw(norm.value_or(raw));
  }
  if (has_tag(e, "$asset_ton")) {
    return Value::make_asset_ton();
  }
  if (has_tag(e, "$asset_jetton")) {
    const std::string raw = jfield(e, "$asset_jetton")->get_string().str();
    auto norm = normalize_raw_address(raw);
    return Value::make_asset_jetton(norm.value_or(raw));
  }
  if (has_tag(e, "$bytes_b64")) {
    auto r = td::base64_decode(jfield(e, "$bytes_b64")->get_string());
    return Value::make_bytes(r.is_ok() ? r.move_as_ok() : std::string{});
  }
  if (has_tag(e, "$cell_b64")) {
    auto raw = td::base64_decode(jfield(e, "$cell_b64")->get_string()).move_as_ok();
    auto cell = vm::std_boc_deserialize(raw).move_as_ok();
    return Value::make_cell(std::move(cell));
  }
  if (has_tag(e, "$obj")) {
    return Value::make_obj(decode_fields(*jfield(e, "$obj")));
  }
  if (has_tag(e, "$list")) {
    std::vector<Value> xs;
    for (const auto &it : jfield(e, "$list")->get_array()) {
      xs.push_back(decode(it));
    }
    return Value::make_list(std::move(xs));
  }
  if (has_tag(e, "$dict")) {
    return Value::make_dict(decode_fields(*jfield(e, "$dict")));
  }
  // Untagged object == plain record (record-literal expectation / map element).
  return Value::make_dict(decode_fields(e));
}

std::map<std::string, Value> decode_env(const Json *env) {
  std::map<std::string, Value> out;
  if (env == nullptr || env->type() != JType::Object) {
    return out;
  }
  for (const auto &kv : env->get_object().field_values_) {
    out.emplace(kv.first.str(), decode(kv.second));
  }
  return out;
}

Lookups decode_lookups(const Json &vec) {
  Lookups out;
  if (const Json *lk = jfield(vec, "lookups"); lk != nullptr && lk->type() == JType::Object) {
    for (const auto &kv : lk->get_object().field_values_) {
      out.emplace(kv.first.str(), decode(kv.second));
    }
  }
  return out;
}

// Compare an evaluation result against the vector's `expect`. Shared by the
// interpreter (--vectors) and generated (--vectors-gen) runners so both paths
// are held to the identical bar.
bool check_result(const Json &vec, const EvalResult &res, std::string &detail) {
  const Json *expect = jfield(vec, "expect");
  if (has_tag(*expect, "$fault")) {
    if (!res.faulted) {
      detail = "expected fault, got " + res.value.describe();
    }
    return res.faulted;
  }
  if (res.faulted) {
    detail = "unexpected fault: " + res.message;
    return false;
  }
  Value expected = decode(*expect);
  if (!structural_equal(res.value, expected)) {
    detail = "got " + res.value.describe() + ", expected " + expected.describe();
    return false;
  }
  return true;
}

// Read + JSON-decode a vectors file; leaves the buffer alive in `buf`.
int load_vectors_json(const std::string &path, std::string &buf, Json &root) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot open vectors file: %s\n", path.c_str());
    return 2;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  buf = ss.str();  // must outlive the decoded tree (Slices point in)
  auto r_root = td::json_decode(td::MutableSlice(buf));
  if (r_root.is_error()) {
    std::fprintf(stderr, "json parse error: %s\n", r_root.error().message().c_str());
    return 2;
  }
  root = r_root.move_as_ok();
  return 0;
}

}  // namespace

Value decode_vector_value(const td::JsonValue &e) {
  return decode(e);
}

int run_vectors(const std::string &path) {
  std::string buf;
  Json root;
  if (int rc = load_vectors_json(path, buf, root); rc != 0) {
    return rc;
  }
  const Json *vectors = jfield(root, "vectors");
  if (vectors == nullptr || vectors->type() != JType::Array) {
    std::fprintf(stderr, "vectors file has no `vectors` array\n");
    return 2;
  }

  int total = 0;
  int passed = 0;
  for (const auto &vec : vectors->get_array()) {
    total++;
    const std::string name = jfield(vec, "name")->get_string().str();
    Env env = decode_env(jfield(vec, "env"));
    Lookups lookups = decode_lookups(vec);

    ExprEval ev(env, lookups);
    EvalResult res = ev.evaluate(*jfield(vec, "expr"));

    std::string detail;
    if (check_result(vec, res, detail)) {
      passed++;
      std::printf("PASS %s\n", name.c_str());
    } else {
      std::printf("FAIL %s -- %s\n", name.c_str(), detail.c_str());
    }
  }

  std::printf("\n%d/%d vectors passed (interpreter)\n", passed, total);
  return passed == total ? 0 : 1;
}

int run_vectors_gen(const std::string &path) {
  std::string buf;
  Json root;
  if (int rc = load_vectors_json(path, buf, root); rc != 0) {
    return rc;
  }
  const Json *vectors = jfield(root, "vectors");
  if (vectors == nullptr || vectors->type() != JType::Array) {
    std::fprintf(stderr, "vectors file has no `vectors` array\n");
    return 2;
  }
  const std::vector<GenVec> &gen = gen_vectors();
  const auto &arr = vectors->get_array();
  if (gen.size() != arr.size()) {
    std::fprintf(stderr,
                 "generated table (%zu) out of sync with vectors file (%zu) — regenerate\n",
                 gen.size(), arr.size());
    return 2;
  }

  int total = 0;
  int passed = 0;
  for (std::size_t i = 0; i < arr.size(); i++) {
    total++;
    const Json &vec = arr[i];
    const std::string name = jfield(vec, "name")->get_string().str();
    if (name != gen[i].name) {
      std::fprintf(stderr, "vector %zu name mismatch ('%s' vs generated '%s') — regenerate\n",
                   i, name.c_str(), gen[i].name);
      return 2;
    }
    Env env = decode_env(jfield(vec, "env"));
    Lookups lookups = decode_lookups(vec);

    EvalResult res = gen[i].fn(env, lookups);  // GENERATED code path

    std::string detail;
    if (check_result(vec, res, detail)) {
      passed++;
      std::printf("PASS %s\n", name.c_str());
    } else {
      std::printf("FAIL %s -- %s\n", name.c_str(), detail.c_str());
    }
  }

  std::printf("\n%d/%d vectors passed (generated)\n", passed, total);
  return passed == total ? 0 : 1;
}

}  // namespace mch
