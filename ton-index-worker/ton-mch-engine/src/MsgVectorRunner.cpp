// --msg-vectors runner (see MsgVectorRunner.h).
#include "MsgVectorRunner.h"

#include "MsgParse.h"
#include "Value.h"

#include "td/utils/JsonBuilder.h"
#include "td/utils/base64.h"
#include "vm/boc.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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

}  // namespace

int run_msg_vectors(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot open msg-vectors file: %s\n", path.c_str());
    return 2;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  std::string buf = ss.str();  // must outlive the decoded tree
  auto r_root = td::json_decode(td::MutableSlice(buf));
  if (r_root.is_error()) {
    std::fprintf(stderr, "json parse error: %s\n", r_root.error().message().c_str());
    return 2;
  }
  Json root = r_root.move_as_ok();
  const Json *cases = jfield(root, "cases");
  if (cases == nullptr || cases->type() != JType::Array) {
    std::fprintf(stderr, "msg-vectors file has no `cases` array\n");
    return 2;
  }

  int total = 0;
  int passed = 0;
  for (const auto &c : cases->get_array()) {
    const Json *jt = jfield(c, "type");
    const Json *jb = jfield(c, "body");
    const Json *je = jfield(c, "expect");
    if (jt == nullptr || jb == nullptr || je == nullptr) {
      std::fprintf(stderr, "FAIL malformed case\n");
      total++;
      continue;
    }
    std::string type = jt->get_string().str();
    bool want_ok = je->get_string() == "ok";
    total++;

    auto r_raw = td::base64_decode(jb->get_string());
    bool got_ok = false;
    std::string why;
    if (r_raw.is_error()) {
      why = "base64";
    } else {
      auto r_cell = vm::std_boc_deserialize(r_raw.move_as_ok());
      if (r_cell.is_error()) {
        why = "boc";
      } else {
        auto r = parse_message_body(type, r_cell.move_as_ok());
        got_ok = r.is_ok();
        if (!got_ok) {
          why = r.error().message().str();
        }
      }
    }
    if (got_ok == want_ok) {
      passed++;
    } else {
      std::printf("FAIL %s expect=%s got=%s (%s)\n", type.c_str(), want_ok ? "ok" : "fail",
                  got_ok ? "ok" : "fail", why.c_str());
    }
  }
  std::printf("MSG-VECTORS %d/%d %s\n", passed, total, passed == total ? "ALL PASS" : "FAILURES");
  return passed == total ? 0 : 1;
}

}  // namespace mch
