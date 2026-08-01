// --msg-vectors / --msg-vectors-regen runner (see MsgVectorRunner.h).
#include "MsgVectorRunner.h"

#include "MsgParse.h"
#include "fixtures/Render.h"

#include "td/utils/JsonBuilder.h"
#include "td/utils/base64.h"
#include "vm/boc.h"

#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace mch {

namespace {

using Json = td::JsonValue;
using JType = td::JsonValue::Type;

struct Case {
  std::string type;
  std::string body;
  std::string expect;
  std::optional<std::string> expect_value;
};

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

std::string json_quote(const std::string &s) {
  std::string out = "\"";
  char escaped[8];
  for (unsigned char c : s) {
    switch (c) {
      case '\"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
          out += escaped;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out + "\"";
}

td::Result<Value> parse_case(const Case &c) {
  auto r_raw = td::base64_decode(td::Slice(c.body));
  if (r_raw.is_error()) {
    return td::Status::Error("base64: " + r_raw.error().message().str());
  }
  auto r_cell = vm::std_boc_deserialize(r_raw.move_as_ok());
  if (r_cell.is_error()) {
    return td::Status::Error("boc: " + r_cell.error().message().str());
  }
  return parse_message_body(c.type, r_cell.move_as_ok());
}

bool decode_case(const Json &jcase, Case &out, std::string &why) {
  const Json *jt = jfield(jcase, "type");
  const Json *jb = jfield(jcase, "body");
  const Json *je = jfield(jcase, "expect");
  const Json *jv = jfield(jcase, "expect_value");
  if (jt == nullptr || jb == nullptr || je == nullptr || jt->type() != JType::String ||
      jb->type() != JType::String || je->type() != JType::String ||
      (jv != nullptr && jv->type() != JType::String)) {
    why = "required fields must be strings";
    return false;
  }
  out.type = jt->get_string().str();
  out.body = jb->get_string().str();
  out.expect = je->get_string().str();
  if (out.expect != "ok" && out.expect != "fail") {
    why = "`expect` must be `ok` or `fail`";
    return false;
  }
  if (jv != nullptr) {
    out.expect_value = jv->get_string().str();
  }
  if (out.expect == "fail" && out.expect_value) {
    why = "a fail case must not have `expect_value`";
    return false;
  }
  return true;
}

bool write_regen(const std::string &path, td::Slice note, const std::vector<Case> &cases) {
  const std::string tmp = path + ".tmp";
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out << "{\n  \"note\": " << json_quote(note.str()) << ",\n  \"cases\": [\n";
  for (std::size_t i = 0; i < cases.size(); i++) {
    const Case &c = cases[i];
    out << "    {\n"
        << "      \"type\": " << json_quote(c.type) << ",\n"
        << "      \"body\": " << json_quote(c.body) << ",\n"
        << "      \"expect\": " << json_quote(c.expect);
    if (c.expect_value) {
      out << ",\n      \"expect_value\": " << json_quote(*c.expect_value);
    }
    out << "\n    }" << (i + 1 == cases.size() ? "\n" : ",\n");
  }
  out << "  ]\n}\n";
  out.close();
  if (!out || std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

int run_msg_vectors_impl(const std::string &path, bool regen) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot open msg-vectors file: %s\n", path.c_str());
    return 2;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  in.close();
  std::string buf = ss.str();  // must outlive the decoded tree
  auto r_root = td::json_decode(td::MutableSlice(buf));
  if (r_root.is_error()) {
    std::fprintf(stderr, "json parse error: %s\n", r_root.error().message().c_str());
    return 2;
  }
  Json root = r_root.move_as_ok();
  const Json *jcases = jfield(root, "cases");
  const Json *jnote = jfield(root, "note");
  if (jcases == nullptr || jcases->type() != JType::Array || jnote == nullptr ||
      jnote->type() != JType::String) {
    std::fprintf(stderr, "msg-vectors file needs string `note` and array `cases`\n");
    return 2;
  }

  int passed = 0;
  int values = 0;
  std::vector<Case> cases;
  cases.reserve(jcases->get_array().size());
  for (const auto &jcase : jcases->get_array()) {
    Case c;
    std::string malformed;
    if (!decode_case(jcase, c, malformed)) {
      std::fprintf(stderr, "FAIL malformed case: %s\n", malformed.c_str());
      cases.push_back(std::move(c));
      continue;
    }
    auto r_value = parse_case(c);
    bool got_ok = r_value.is_ok();
    bool want_ok = c.expect == "ok";
    std::string actual = got_ok ? render_parse_value(r_value.ok()) : std::string();
    bool value_ok = !c.expect_value || (got_ok && actual == *c.expect_value);
    if (regen && want_ok && got_ok) {
      c.expect_value = actual;
      value_ok = true;
    }
    if (got_ok == want_ok && value_ok) {
      passed++;
      if (c.expect_value) values++;
    } else if (got_ok != want_ok) {
      std::printf("FAIL %s expect=%s got=%s (%s)\n", c.type.c_str(), c.expect.c_str(),
                  got_ok ? "ok" : "fail",
                  got_ok ? "" : r_value.error().message().c_str());
    } else {
      std::printf("FAIL %s value mismatch\n  expect: %s\n  actual: %s\n", c.type.c_str(),
                  c.expect_value->c_str(), actual.c_str());
    }
    cases.push_back(std::move(c));
  }

  const int total = static_cast<int>(jcases->get_array().size());
  if (passed != total) {
    std::printf("MSG-VECTORS%s %d/%d FAILURES\n", regen ? "-REGEN" : "", passed, total);
    return 1;
  }
  if (regen) {
    if (!write_regen(path, jnote->get_string(), cases)) {
      std::fprintf(stderr, "cannot atomically rewrite msg-vectors file: %s\n", path.c_str());
      return 2;
    }
    std::printf("MSG-VECTORS-REGEN %d values / %d cases WRITTEN\n", values, total);
  } else {
    std::printf("MSG-VECTORS %d/%d ALL PASS (%d exact values)\n", passed, total, values);
  }
  return 0;
}

}  // namespace

int run_msg_vectors(const std::string &path) { return run_msg_vectors_impl(path, false); }

int regen_msg_vectors(const std::string &path) { return run_msg_vectors_impl(path, true); }

}  // namespace mch
