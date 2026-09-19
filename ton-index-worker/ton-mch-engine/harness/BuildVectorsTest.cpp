#include "BuildVectorsTest.h"

#include "BlockTree.h"
#include "BuildRuntime.h"
#include "GenBuildsVectors.h"
#include "fixtures/Render.h"
#include "ExprVectorsTest.h"
#include "btypes_gen.h"

#include "td/utils/JsonBuilder.h"
#include "td/utils/crypto.h"

#include <exception>

#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace mch {

namespace {

using Json = td::JsonValue;
using JType = td::JsonValue::Type;

const Json *jf(const Json &e, td::Slice name) {
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

std::string jstr(const Json &e, const char *name, const std::string &dflt = {}) {
  const Json *f = jf(e, td::Slice(name));
  return (f != nullptr && f->type() == JType::String) ? f->get_string().str() : dflt;
}

std::int64_t jint(const Json &e, const char *name, std::int64_t dflt = 0) {
  const Json *f = jf(e, td::Slice(name));
  return (f != nullptr && f->type() == JType::Number) ? std::stoll(f->get_number().str()) : dflt;
}

bool jbool(const Json &e, const char *name, bool dflt = false) {
  const Json *f = jf(e, td::Slice(name));
  return (f != nullptr && f->type() == JType::Boolean) ? f->get_boolean() : dflt;
}

// Backing storage for the synthetic blocks of ONE case (Block/EventNode point
// into Message/Transaction; deque keeps addresses stable).
struct CaseStore {
  std::deque<Transaction> txs;
  std::deque<Message> msgs;
  std::deque<EventNode> nodes;
  BlockArena arena;
};

std::optional<std::string> opt_str(const Json &e, const char *name) {
  const Json *f = jf(e, td::Slice(name));
  if (f == nullptr || f->type() != JType::String) {
    return std::nullopt;
  }
  return f->get_string().str();
}

std::optional<std::int64_t> opt_int(const Json &e, const char *name) {
  const Json *f = jf(e, td::Slice(name));
  if (f == nullptr || f->type() != JType::Number) {
    return std::nullopt;
  }
  return std::stoll(f->get_number().str());
}

Block *decode_block(const Json &b, CaseStore &store) {
  Transaction &tx = store.txs.emplace_back();
  Message &msg = store.msgs.emplace_back();
  if (const Json *jtx = jf(b, "tx"); jtx != nullptr) {
    tx.hash = jstr(*jtx, "hash");
    tx.lt = jint(*jtx, "lt");
    tx.now = jint(*jtx, "now");
    tx.account = jstr(*jtx, "account");
    tx.aborted = jbool(*jtx, "aborted");
    tx.orig_status = jstr(*jtx, "orig_status", "active");
    tx.end_status = jstr(*jtx, "end_status", "active");
    tx.compute_exit_code = opt_int(*jtx, "compute_exit_code");
  }
  if (const Json *jm = jf(b, "msg"); jm != nullptr) {
    msg.msg_hash = jstr(*jm, "msg_hash");
    msg.tx_hash = jstr(*jm, "tx_hash");
    msg.tx_lt = jint(*jm, "tx_lt");
    msg.direction = jstr(*jm, "direction", "in");
    msg.source = opt_str(*jm, "source");
    msg.destination = opt_str(*jm, "destination");
    msg.value = opt_int(*jm, "value");
    msg.opcode = opt_int(*jm, "opcode");
    msg.created_lt = opt_int(*jm, "created_lt");
    msg.created_at = opt_int(*jm, "created_at");
    if (const Json *jb = jf(*jm, "bounce"); jb != nullptr && jb->type() == JType::Boolean) {
      msg.bounce = jb->get_boolean();
    }
    msg.bounced = jbool(*jm, "bounced");
  }
  msg.tx = &tx;
  if (const Json *body = jf(b, "body_b64"); body != nullptr && body->type() == JType::String) {
    MsgContent c;
    c.body = body->get_string().str();  // stored base64, like TraceLoader
    msg.content = std::move(c);
  }

  EventNode &node = store.nodes.emplace_back();
  node.msg = &msg;
  node.tx = &tx;

  Block *blk = store.arena.make(jstr(b, "btype", mch::btype::kCallContract));
  if (const Json *op = jf(b, "opcode"); op != nullptr && op->type() == JType::Number) {
    blk->opcode = static_cast<std::uint32_t>(std::stoll(op->get_number().str()) & 0xFFFFFFFF);
  }
  blk->failed = jbool(b, "failed");
  blk->broken = jbool(b, "broken");
  if (const Json *data = jf(b, "data"); data != nullptr) {
    blk->data = decode_vector_value(*data);
  }
  blk->event_nodes.push_back(&node);
  blk->min_lt = msg.created_lt ? *msg.created_lt : tx.lt;
  return blk;
}

class VecLookupTable : public LookupTable {
 public:
  std::map<std::string, Value> kinds;
  bool has(const std::string &kind) const override { return kinds.count(kind) != 0; }
  Value get(const std::string &kind, const std::vector<Value> &) const override {
    auto it = kinds.find(kind);
    return it != kinds.end() ? it->second : Value::null();
  }
};

// Slot value: $block / null / array of ($block | null).
Value decode_slot(const Json &v, CaseStore &store,
                  std::map<std::string, const Block *> &by_name, const std::string &name) {
  if (v.type() == JType::Null) {
    return Value::null();
  }
  if (v.type() == JType::Array) {
    std::vector<Value> xs;
    for (const auto &el : v.get_array()) {
      if (el.type() == JType::Null) {
        xs.push_back(Value::null());
      } else {
        const Block *b = decode_block(el, store);
        xs.push_back(Value::make_block(b));
      }
    }
    return Value::make_list(std::move(xs));
  }
  const Block *b = decode_block(v, store);
  by_name[name] = b;
  return Value::make_block(b);
}

bool check_case(const Json &vec, const BuildOutcome &o, std::string &why) {
  const Json *expect = jf(vec, "expect");
  if (expect == nullptr) {
    why = "case has no `expect`";
    return false;
  }
  if (jbool(*expect, "rejected")) {
    if (!o.is_rejected) {
      why = "expected rejection, got accepted (data " + o.data.describe() + ")";
      return false;
    }
    return true;
  }
  if (o.is_rejected) {
    why = "unexpected rejection (" + o.reject_reason + ")";
    return false;
  }
  const std::string ebtype = jstr(*expect, "btype");
  if (ebtype != o.btype) {
    why = "btype '" + o.btype + "' != expected '" + ebtype + "'";
    return false;
  }
  if (jbool(*expect, "failed") != o.failed || jbool(*expect, "broken") != o.broken) {
    why = std::string("failed/broken = ") + (o.failed ? "t" : "f") + "/" + (o.broken ? "t" : "f");
    return false;
  }
  if (const Json *edata = jf(*expect, "data"); edata != nullptr) {
    Value expected = decode_vector_value(*edata);
    if (!structural_equal(o.data, expected)) {
      why = "data " + o.data.describe() + " != expected " + expected.describe();
      return false;
    }
  }
  return true;
}

int load_build_vectors(const std::string &path, std::string &buf, Json &root, bool check_sha) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
    return 2;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  buf = ss.str();  // must outlive the decoded tree (Slices point in)
  if (check_sha) {
    std::string source_sha;
    unsigned char digest[32];
    td::sha256(td::Slice(buf), td::MutableSlice(digest, 32));
    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
      source_sha += hex[digest[i] >> 4];
      source_sha += hex[digest[i] & 0xF];
    }
    if (source_sha != gen_builds_vectors_source_sha()) {
      std::fprintf(stderr,
                   "generated build-vectors table was built from a DIFFERENT document (table sha "
                   "%s, loaded %s) — the CMake build regenerates it from ir/build_vectors.json; "
                   "rebuild ton-mch-engine or pass that file\n",
                   gen_builds_vectors_source_sha(), source_sha.c_str());
      return 2;
    }
  }
  auto r_root = td::json_decode(td::MutableSlice(buf));
  if (r_root.is_error()) {
    std::fprintf(stderr, "json parse error: %s\n", r_root.error().message().c_str());
    return 2;
  }
  root = r_root.move_as_ok();
  const Json *cases = jf(root, "build_vectors");
  if (cases == nullptr || cases->type() != JType::Array) {
    std::fprintf(stderr, "file has no `build_vectors` array\n");
    return 2;
  }
  return 0;
}

template <typename Run>
int run_build_vector_cases(const td::JsonArray &arr, const char *label, Run &&run) {
  const auto &table = gen_builds_vectors();
  if (table.size() != arr.size()) {
    std::fprintf(stderr,
                 "generated build-vectors table has %zu fns for %zu cases — rebuild "
                 "ton-mch-engine (CMake regenerates it from ir/build_vectors.json)\n",
                 table.size(), arr.size());
    return 2;
  }

  int total = 0;
  int passed = 0;
  for (std::size_t i = 0; i < arr.size(); i++) {
    const Json &vec = arr[i];
    const std::string name = jstr(vec, "name");
    total++;
    if (std::string(table[i].name) != name || table[i].id != static_cast<int>(i)) {
      std::printf("FAIL %s: generated table mismatch (have id %d '%s')\n", name.c_str(),
                  table[i].id, table[i].name);
      continue;
    }

    CaseStore store;
    std::map<std::string, const Block *> blocks_by_name;
    std::vector<Value> slots;
    const Json *matcher = jf(vec, "matcher");
    const Json *env = jf(vec, "env");
    const Json *jslots = env != nullptr ? jf(*env, "slots") : nullptr;
    if (const Json *caps = matcher != nullptr ? jf(*matcher, "captures") : nullptr;
        caps != nullptr && caps->type() == JType::Array) {
      for (const auto &c : caps->get_array()) {
        const std::string cname = jstr(c, "name");
        const Json *sv = jslots != nullptr ? jf(*jslots, td::Slice(cname)) : nullptr;
        slots.push_back(sv != nullptr ? decode_slot(*sv, store, blocks_by_name, cname)
                                      : Value::null());
      }
    }

    VecLookupTable lookups;
    if (const Json *lk = env != nullptr ? jf(*env, "lookups") : nullptr;
        lk != nullptr && lk->type() == JType::Object) {
      for (const auto &kv : lk->get_object().field_values_) {
        lookups.kinds.emplace(kv.first.str(), decode_vector_value(kv.second));
      }
    }

    BuildEnv benv;
    benv.slots = slots.data();
    benv.n_slots = slots.size();
    benv.lookups = &lookups;
    if (env != nullptr) {
      const std::string anchor_name = jstr(*env, "anchor");
      if (auto it = blocks_by_name.find(anchor_name); it != blocks_by_name.end()) {
        benv.anchor = it->second;
      }
      std::vector<Value> consumed;
      if (const Json *jc = jf(*env, "consumed"); jc != nullptr && jc->type() == JType::Array) {
        for (const auto &cn : jc->get_array()) {
          auto it = blocks_by_name.find(cn.get_string().str());
          consumed.push_back(it != blocks_by_name.end() ? Value::make_block(it->second)
                                                        : Value::null());
        }
      }
      benv.consumed = Value::make_list(std::move(consumed));
    } else {
      benv.consumed = Value::make_list({});
    }

    BuildOutcome outcome;
    try {
      outcome = run(i, vec, benv);
    } catch (const std::exception &e) {
      std::fprintf(stderr, "%s\n", e.what());
      return 2;
    }
    std::string why;
    if (check_case(vec, outcome, why)) {
      std::printf("PASS %s\n", name.c_str());
      passed++;
    } else {
      std::printf("FAIL %s: %s\n", name.c_str(), why.c_str());
    }
  }
  std::printf("\n%d/%d build vectors passed (%s)\n", passed, total, label);
  return passed == total ? 0 : 1;
}

}  // namespace

int run_build_vectors(const std::string &path) {
  std::string buf;
  Json root;
  if (int rc = load_build_vectors(path, buf, root, /*check_sha=*/true); rc != 0) {
    return rc;
  }
  return run_build_vector_cases(jf(root, "build_vectors")->get_array(), "generated",
                                [](std::size_t i, const Json &, BuildEnv &benv) {
                                  return gen_builds_vectors()[i].fn(benv);
                                });
}

}  // namespace mch
