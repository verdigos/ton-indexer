#include "BuildDriver.h"

#include "BlockTree.h"
#include "GenBuilds.h"
#include "HostRegistry.h"
#include "MsgParse.h"

#include "td/utils/base64.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <stdexcept>

namespace mch {

namespace {

std::string hex_upper(const unsigned char *p, size_t n) {
  static const char *digits = "0123456789ABCDEF";
  std::string out(n * 2, '0');
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = digits[p[i] >> 4];
    out[2 * i + 1] = digits[p[i] & 0xF];
  }
  return out;
}

// Canonical float text shared with the Python twin ("%.17g" both sides). Chosen
// over shortest-round-trip (repr / to_chars) because those disagree on round
// numbers (repr keeps a trailing ".0", to_chars drops it); %.17g is one agreed
// format, byte-verified on the corpus values.
std::string fmt_g17(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return std::string(buf);
}

}  // namespace

std::string block_key(const Block *b) {
  std::string key = b->btype;
  if (b->opcode) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%08x", *b->opcode);
    key += buf;
  }
  key += "@" + std::to_string(b->min_lt);
  return key;
}

std::string render_value(const Value &v) {
  switch (v.t) {
    case VType::Null:
      return "null";
    case VType::Bool:
      return v.boolean ? "true" : "false";
    case VType::Int:
      return v.num.is_null() ? "nan" : v.num->to_dec_string();
    case VType::Float:
      // Bare Python float: "float:" + shortest-safe %.17g (byte-matched by the
      // twin's "float:%.17g" % v). Renderable now, so the NFT purchase/shaper
      // path fires instead of TRACE_FAILing.
      return "float:" + fmt_g17(v.dnum);
    case VType::Amount:
      if (v.amount_float) {
        return "amount:" + fmt_g17(v.dnum);  // Python Amount(float): str(float)
      }
      return "amount:" + (v.num.is_null() ? std::string("nan") : v.num->to_dec_string());
    case VType::Str:
      return "str:" + v.str;
    case VType::Bytes:
      return "b64:" + td::base64_encode(td::Slice(v.str));
    case VType::Account:
      return v.addr_none ? "addr_none" : v.str;
    case VType::Asset:
      return v.is_ton ? "asset:TON" : "asset:" + v.str;
    case VType::Cell: {
      if (v.cell.is_null()) {
        return "cell:null";
      }
      auto h = v.cell->get_hash();
      return "cell:" + hex_upper(h.as_slice().ubegin(), h.as_slice().size());
    }
    case VType::List: {
      std::string out = "[";
      for (std::size_t i = 0; i < v.items->size(); i++) {
        if (i) out += ",";
        out += render_value((*v.items)[i]);
      }
      return out + "]";
    }
    case VType::Dict:
    case VType::Obj: {
      std::vector<std::string> keys;
      for (const auto &kv : *v.fields) {
        keys.push_back(kv.first);
      }
      std::sort(keys.begin(), keys.end());
      std::string out = v.t == VType::Obj ? "obj{" : "{";
      bool first = true;
      for (const auto &k : keys) {
        if (!first) out += ",";
        first = false;
        out += k + "=" + render_value(*v.field(k));
      }
      return out + "}";
    }
    case VType::Block:
      return "block:" + (v.block != nullptr ? block_key(v.block) : std::string("null"));
  }
  return "?";
}

std::string build_skip_reason(const CompiledMatcher &m) {
  if (m.unrunnable) {
    return m.skip_reason;  // match-phase pred skip
  }
  if (!m.has_build_program) {
    return m.ref_builders.empty() ? "no_build_program" : "builder";
  }
  std::vector<std::string> missing_types;
  for (const std::string &t : m.ref_msgtypes) {
    if (message_parsers().find(t) == message_parsers().end()) {
      missing_types.push_back(t);
    }
  }
  std::vector<std::string> missing_lookups;
  for (const std::string &l : m.ref_lookups) {
    if (FixtureLookupSource::kinds().count(l) == 0) {
      missing_lookups.push_back(l);
    }
  }
  std::vector<std::string> missing_fns;
  for (const std::string &fn : m.ref_fns) {
    if (host_fns().find(fn) == host_fns().end()) {
      missing_fns.push_back(fn);
    }
  }
  std::vector<std::string> missing_shapers;
  for (const std::string &s : m.ref_shapers) {
    if (host_shapers().find(s) == host_shapers().end()) {
      missing_shapers.push_back(s);
    }
  }
  std::string reason;
  auto add = [&reason](const std::string &tag, const std::vector<std::string> &names) {
    if (names.empty()) {
      return;
    }
    if (!reason.empty()) reason += ";";
    reason += tag + ":";
    for (std::size_t i = 0; i < names.size(); i++) {
      if (i) reason += ",";
      reason += names[i];
    }
  };
  add("types", missing_types);
  add("lookups", missing_lookups);
  add("fns", missing_fns);
  add("shaper", missing_shapers);
  return reason;  // empty == included
}

void warn_missing_artifact_parsers(const LoadedIr &ir) {
  const auto &parsers = message_parsers();
  std::set<std::string> missing;
  for (const CompiledMatcher &m : ir.matchers) {
    for (const std::string &t : m.ref_msgtypes) {
      if (parsers.find(t) == parsers.end()) {
        missing.insert(t);
      }
    }
  }
  if (missing.empty()) {
    return;
  }
  std::string joined;
  for (const std::string &t : missing) {
    if (!joined.empty()) joined += ", ";
    joined += t;
  }
  std::fprintf(stderr,
               "WARNING: artifact references %zu message type(s) with no registered C++ parser "
               "(matchers that parse them will be build-skipped): %s\n",
               missing.size(), joined.c_str());
}

std::vector<Value> slots_from_captures(const std::vector<Capture> &captures) {
  std::vector<Value> slots;
  for (const Capture &c : captures) {
    if (c.is_list) {
      std::vector<Value> xs;
      for (Block *b : c.vals) {
        xs.push_back(b != nullptr ? Value::make_block(b) : Value::null());
      }
      slots.push_back(Value::make_list(std::move(xs)));
    } else {
      Block *b = c.vals.empty() ? nullptr : c.vals[0];
      slots.push_back(b != nullptr ? Value::make_block(b) : Value::null());
    }
  }
  return slots;
}

BuildOutcome run_two_phase(BuildOutcome (*fn)(BuildEnv &), const Block *anchor,
                           const std::vector<Value> &slots, const Value &consumed,
                           const std::set<std::string> &kinds, const LookupSource &src) {
  auto fresh_env = [&](const LookupTable *table) {
    BuildEnv env;
    env.anchor = anchor;
    env.slots = slots.data();
    env.n_slots = slots.size();
    env.consumed = consumed;
    env.lookups = table;
    return env;
  };

  std::map<std::string, Value> fetched;
  bool converged = false;
  for (int round = 0; round < 8; round++) {
    CollectingLookupTable coll(kinds, fetched);
    BuildEnv env = fresh_env(&coll);
    fn(env);  // dry pass, outcome discarded
    if (coll.misses.empty()) {
      converged = true;
      break;
    }
    for (const auto &[key, ka] : coll.misses) {
      fetched.emplace(key, src.fetch(ka.first, ka.second));
    }
  }
  // Python resolves any lookup-arg-of-lookup depth inline, so an unconverged
  // fixpoint here is a genuine divergence, not a benign truncation — do not
  // proceed with a partial table (silent wrongness). Throw to the classify
  // trace boundary (Python's per-trace fallback). Today's artifacts have max
  // lookup depth 2 and converge in <=2 rounds, so this never fires.
  if (!converged) {
    throw std::runtime_error("two-phase lookup did not converge after 8 rounds");
  }

  FilledLookupTable filled(kinds, std::move(fetched));
  BuildEnv env = fresh_env(&filled);
  return fn(env);
}

const std::vector<GenBuild> *select_gen_builds(const std::string &sha, const char **which) {
  if (sha == gen_builds_ir_source_sha()) {
    if (which != nullptr) {
      *which = "ir";
    }
    return &gen_builds_ir();
  }
  if (sha == gen_builds_testbed_source_sha()) {
    if (which != nullptr) {
      *which = "testbed";
    }
    return &gen_builds_testbed();
  }
  return nullptr;
}

}  // namespace mch
