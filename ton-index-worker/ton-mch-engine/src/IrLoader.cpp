#include "IrLoader.h"

#include "td/utils/JsonBuilder.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace mch {

namespace {

using Json = td::JsonValue;
using JType = td::JsonValue::Type;

const std::unordered_set<std::string> kBuiltins = {
    "account", "amount", "asset", "ton_asset", "addr_none", "b64",
    "first", "last", "len", "sum", "zip", "map"};

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

bool has(const Json &e, td::Slice name) { return jfield(e, name) != nullptr; }

std::string jstr(const Json &e, td::Slice name, const std::string &dflt = {}) {
  const Json *f = jfield(e, name);
  return (f != nullptr && f->type() == JType::String) ? f->get_string().str() : dflt;
}

std::int64_t jint(const Json &e, td::Slice name, std::int64_t dflt = 0) {
  const Json *f = jfield(e, name);
  if (f == nullptr || f->type() != JType::Number) {
    return dflt;
  }
  return std::stoll(f->get_number().str());
}

bool jbool(const Json &e, td::Slice name, bool dflt = false) {
  const Json *f = jfield(e, name);
  return (f != nullptr && f->type() == JType::Boolean) ? f->get_boolean() : dflt;
}

// Node ids referenced by a node record, in the fixed order Python's _edge_refs
// uses (child, children..., parent, branches..., step, exit) — remap discovery
// order depends on it.
std::vector<int> edge_refs(const Json &rec) {
  std::vector<int> out;
  if (has(rec, "child")) {
    out.push_back(static_cast<int>(jint(rec, "child")));
  }
  if (const Json *ch = jfield(rec, "children"); ch != nullptr && ch->type() == JType::Array) {
    for (const auto &c : ch->get_array()) {
      out.push_back(static_cast<int>(std::stoll(c.get_number().str())));
    }
  }
  if (has(rec, "parent")) {
    out.push_back(static_cast<int>(jint(rec, "parent")));
  }
  if (const Json *br = jfield(rec, "branches"); br != nullptr && br->type() == JType::Array) {
    for (const auto &b : br->get_array()) {
      out.push_back(static_cast<int>(std::stoll(b.get_number().str())));
    }
  }
  if (has(rec, "step")) {
    out.push_back(static_cast<int>(jint(rec, "step")));
  }
  if (has(rec, "exit")) {
    out.push_back(static_cast<int>(jint(rec, "exit")));
  }
  return out;
}

NodeKind parse_kind(const std::string &k) {
  if (k == "contract") return NodeKind::Contract;
  if (k == "block_type") return NodeKind::BlockType;
  if (k == "pred") return NodeKind::Pred;
  if (k == "or") return NodeKind::Or;
  if (k == "recursive") return NodeKind::Recursive;
  return NodeKind::Any;
}

// Recursively collect host-fn / lookup / parse-type names from a build_program
// JSON subtree (report only).
void scan_program(const Json &e, CompiledMatcher &m) {
  if (e.type() == JType::Object) {
    const std::string k = jstr(e, "k");
    if (k == "call") {
      std::string fn = jstr(e, "fn");
      if (!fn.empty() && kBuiltins.count(fn) == 0) {
        m.ref_fns.insert(fn);
      }
    } else if (k == "lookup") {
      std::string nm = jstr(e, "name");
      if (!nm.empty()) {
        m.ref_lookups.insert(nm);
      }
    }
    if (jstr(e, "s") == "parse") {
      if (const Json *types = jfield(e, "types"); types != nullptr && types->type() == JType::Array) {
        for (const auto &t : types->get_array()) {
          if (t.type() == JType::String) {
            m.ref_msgtypes.insert(t.get_string().str());
          }
        }
      }
    }
    for (const auto &kv : e.get_object().field_values_) {
      scan_program(kv.second, m);
    }
  } else if (e.type() == JType::Array) {
    for (const auto &it : e.get_array()) {
      scan_program(it, m);
    }
  }
}

// Local reachability over every edge kind (child/parent/step/exit/children/
// branches), mirroring IrMatcher._reachable — cycle-safe, discovery order.
std::vector<int> reachable(const std::vector<CompiledNode> &nodes, int start) {
  std::vector<int> out{start};
  std::unordered_set<int> seen{start};
  std::size_t qi = 0;
  while (qi < out.size()) {
    const CompiledNode &n = nodes[out[qi++]];
    std::vector<int> refs;
    if (n.child >= 0) refs.push_back(n.child);
    if (n.parent >= 0) refs.push_back(n.parent);
    if (n.step >= 0) refs.push_back(n.step);
    if (n.exit >= 0) refs.push_back(n.exit);
    refs.insert(refs.end(), n.children.begin(), n.children.end());
    refs.insert(refs.end(), n.branches.begin(), n.branches.end());
    for (int r : refs) {
      if (r >= 0 && seen.insert(r).second) {
        out.push_back(r);
      }
    }
  }
  return out;
}

td::Status build_matcher(const Json &rec, const std::vector<const Json *> &pool,
                         CompiledMatcher &m) {
  m.name = jstr(rec, "name");
  if (const Json *pr = jfield(rec, "produces"); pr != nullptr && pr->type() == JType::Array) {
    for (const auto &p : pr->get_array()) {
      m.produces.push_back(p.get_string().str());
    }
  }

  const Json *anchor = jfield(rec, "anchor");
  const std::string ak = anchor != nullptr ? jstr(*anchor, "kind") : "";
  if (ak == "opcode_set") {
    m.anchor_kind = AnchorKind::OpcodeSet;
    for (const auto &v : jfield(*anchor, "values")->get_array()) {
      m.anchor_opcodes.insert(static_cast<std::uint32_t>(std::stoll(v.get_number().str()) & 0xFFFFFFFF));
    }
  } else if (ak == "btype") {
    m.anchor_kind = AnchorKind::BType;
    for (const auto &v : jfield(*anchor, "values")->get_array()) {
      m.anchor_btypes.insert(v.get_string().str());
    }
  } else if (ak == "pred") {
    m.anchor_kind = AnchorKind::Pred;
    m.ref_preds.insert(jstr(*anchor, "pred"));
  } else if (ak == "mixed") {
    m.anchor_kind = AnchorKind::Mixed;
    if (const Json *br = jfield(*anchor, "branches"); br != nullptr && br->type() == JType::Array) {
      for (const auto &b : br->get_array()) {
        if (has(b, "where")) {
          m.ref_preds.insert(jstr(b, "where"));
        }
      }
    }
  } else {
    return td::Status::Error("matcher " + m.name + ": bad anchor kind '" + ak + "'");
  }

  // Slots + cards (captures table declaration order).
  std::unordered_map<std::string, int> slot_of;
  if (const Json *caps = jfield(rec, "captures"); caps != nullptr && caps->type() == JType::Array) {
    for (const auto &c : caps->get_array()) {
      std::string cn = jstr(c, "name");
      slot_of[cn] = static_cast<int>(m.slot_names.size());
      m.slot_names.push_back(cn);
      m.cards.push_back(jstr(c, "card"));
    }
  }

  // Reachable subgraph remapped to local ids (root = 0), discovery order over
  // edge_refs — mirrors IrMatcher.__init__.
  int root = static_cast<int>(jint(rec, "root"));
  std::vector<int> order{root};
  std::unordered_map<int, int> remap{{root, 0}};
  std::size_t qi = 0;
  while (qi < order.size()) {
    const Json &nrec = *pool[order[qi++]];
    for (int ref : edge_refs(nrec)) {
      if (ref < 0 || ref >= static_cast<int>(pool.size())) {
        return td::Status::Error("matcher " + m.name + ": node ref out of range");
      }
      if (remap.find(ref) == remap.end()) {
        remap[ref] = static_cast<int>(order.size());
        order.push_back(ref);
      }
    }
  }

  auto rm = [&](int old) { return old < 0 ? -1 : remap[old]; };
  for (int old : order) {
    const Json &nrec = *pool[old];
    CompiledNode cn;
    cn.global_id = old;
    cn.kind = parse_kind(jstr(nrec, "kind"));
    if (has(nrec, "opcode")) {
      cn.opcode = static_cast<std::uint32_t>(jint(nrec, "opcode") & 0xFFFFFFFF);
      cn.has_opcode = true;
    }
    cn.btype = jstr(nrec, "btype");
    if (cn.kind == NodeKind::Pred) {
      cn.pred_name = jstr(nrec, "pred");
    }
    cn.where_name = jstr(nrec, "where");
    if (const Json *wf = jfield(nrec, "where_expr"); wf != nullptr) {
      cn.has_where_expr = true;
      // Re-encode the subtree into a self-owned buffer, decode once: the
      // decoded JsonValue slices point into WhereExpr::buf, which lives (and
      // never moves) behind the shared_ptr, outliving the artifact document.
      auto we = std::make_shared<WhereExpr>();
      {
        auto sbuf = td::StackAllocator::alloc(1 << 16);
        td::JsonBuilder jb(td::StringBuilder(sbuf.as_slice(), true), -1);
        auto scope = jb.enter_value();
        wf->store(&scope);
        we->buf = jb.string_builder().as_cslice().str();
      }
      auto r_expr = td::json_decode(td::MutableSlice(we->buf));
      if (r_expr.is_error()) {
        return td::Status::Error("matcher " + m.name + ": where_expr re-decode failed: " +
                                 r_expr.error().message().str());
      }
      we->expr = r_expr.move_as_ok();
      cn.where_expr = std::move(we);
    }
    std::string cap = jstr(nrec, "capture");
    cn.slot = (!cap.empty() && slot_of.count(cap)) ? slot_of[cap] : -1;
    cn.optional = jbool(nrec, "optional");
    cn.child = has(nrec, "child") ? rm(static_cast<int>(jint(nrec, "child"))) : -1;
    if (const Json *ch = jfield(nrec, "children"); ch != nullptr && ch->type() == JType::Array) {
      for (const auto &c : ch->get_array()) {
        cn.children.push_back(rm(static_cast<int>(std::stoll(c.get_number().str()))));
      }
    }
    cn.parent = has(nrec, "parent") ? rm(static_cast<int>(jint(nrec, "parent"))) : -1;
    if (const Json *br = jfield(nrec, "branches"); br != nullptr && br->type() == JType::Array) {
      for (const auto &b : br->get_array()) {
        cn.branches.push_back(rm(static_cast<int>(std::stoll(b.get_number().str()))));
      }
    }
    cn.step = has(nrec, "step") ? rm(static_cast<int>(jint(nrec, "step"))) : -1;
    cn.exit = has(nrec, "exit") ? rm(static_cast<int>(jint(nrec, "exit"))) : -1;
    cn.exclusive = jbool(nrec, "exclusive");
    cn.strategy = jstr(nrec, "strategy") == "cyclic" ? RecStrategy::Cyclic : RecStrategy::Frontier;
    m.nodes.push_back(std::move(cn));
  }

  // Recursion ownership + load-time gates (nested recursion, cyclic captures).
  std::set<int> owned;
  for (int nid = 0; nid < static_cast<int>(m.nodes.size()); nid++) {
    const CompiledNode &node = m.nodes[nid];
    if (node.kind != NodeKind::Recursive) {
      continue;
    }
    std::vector<int> reach = reachable(m.nodes, node.step);
    for (int r : reach) {
      if (m.nodes[r].kind == NodeKind::Recursive) {
        return td::Status::Error("matcher " + m.name + ": nested recursion is not supported");
      }
    }
    if (node.strategy == RecStrategy::Cyclic) {
      for (int r : reach) {
        if (m.nodes[r].slot >= 0) {
          return td::Status::Error("matcher " + m.name +
                                   ": captures inside a cyclic recursion body are not supported");
        }
      }
      m.step_slots[nid] = {};
      m.exit_slots[nid] = {};
      continue;
    }
    std::set<int> ss;
    for (int r : reach) {
      if (m.nodes[r].slot >= 0) ss.insert(m.nodes[r].slot);
    }
    m.step_slots[nid] = ss;
    std::set<int> es;
    if (node.exit >= 0) {
      for (int r : reachable(m.nodes, node.exit)) {
        if (m.nodes[r].slot >= 0) es.insert(m.nodes[r].slot);
      }
    }
    m.exit_slots[nid] = es;
    owned.insert(ss.begin(), ss.end());
    owned.insert(es.begin(), es.end());
  }
  m.owned_slots = owned;

  m.include_excess = jbool(rec, "include_excess", true);
  m.include_bounces = jbool(rec, "include_bounces", true);
  m.priority = static_cast<int>(jint(rec, "priority", 100));
  if (has(rec, "builder")) {
    m.ref_builders.insert(jstr(rec, "builder"));
  }
  if (has(rec, "shape")) {
    m.ref_shapers.insert(jstr(rec, "shape"));
  }
  if (const Json *bp = jfield(rec, "build_program"); bp != nullptr) {
    scan_program(*bp, m);
  }

  // Match-phase runnability: named host predicates (anchor + reachable pred /
  // `where` nodes) are the only match-time host dependencies. Inline
  // where_exprs are evaluated by the walker (interpreter or generated code)
  // and no longer make a matcher unrunnable.
  std::set<std::string> pred_names;
  for (const CompiledNode &n : m.nodes) {
    if (n.kind == NodeKind::Pred && !n.pred_name.empty()) {
      pred_names.insert(n.pred_name);
    }
    if (!n.where_name.empty()) {
      pred_names.insert(n.where_name);
    }
  }
  // Anchor-level predicate names already collected in ref_preds for pred/mixed.
  if (m.anchor_kind == AnchorKind::Pred || m.anchor_kind == AnchorKind::Mixed) {
    pred_names.insert(m.ref_preds.begin(), m.ref_preds.end());
  }
  m.ref_preds.insert(pred_names.begin(), pred_names.end());
  if (!pred_names.empty()) {
    m.unrunnable = true;
    std::string reason = "pred:";
    bool first = true;
    for (const std::string &p : pred_names) {
      if (!first) reason += ",";
      first = false;
      reason += p;
    }
    m.skip_reason = reason;
  }
  return td::Status::OK();
}

}  // namespace

td::Result<LoadedIr> load_ir(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return td::Status::Error("cannot open IR artifact: " + path);
  }
  std::stringstream ss;
  ss << in.rdbuf();
  std::string buf = ss.str();

  auto r_root = td::json_decode(td::MutableSlice(buf));
  if (r_root.is_error()) {
    return td::Status::Error("IR json parse error: " + r_root.error().message().str());
  }
  Json root = r_root.move_as_ok();
  if (root.type() != JType::Object) {
    return td::Status::Error("IR artifact root must be an object");
  }
  if (jstr(root, "mch_ir_version") != "0.1-poc") {
    return td::Status::Error("unsupported mch_ir_version (expected 0.1-poc)");
  }
  const Json *nodes = jfield(root, "nodes");
  const Json *matchers = jfield(root, "matchers");
  if (nodes == nullptr || nodes->type() != JType::Array ||
      matchers == nullptr || matchers->type() != JType::Array) {
    return td::Status::Error("IR artifact needs 'nodes' and 'matchers' arrays");
  }

  std::vector<const Json *> pool;
  for (const auto &n : nodes->get_array()) {
    pool.push_back(&n);
  }

  LoadedIr out;
  out.n_nodes = static_cast<int>(pool.size());

  std::vector<CompiledMatcher> compiled;
  for (const auto &rec : matchers->get_array()) {
    CompiledMatcher m;
    TRY_STATUS(build_matcher(rec, pool, m));
    compiled.push_back(std::move(m));
  }

  // registration_order (default: identity), then stable-sort by priority.
  std::vector<int> reg_order;
  if (const Json *ro = jfield(root, "registration_order"); ro != nullptr && ro->type() == JType::Array) {
    for (const auto &mi : ro->get_array()) {
      reg_order.push_back(static_cast<int>(std::stoll(mi.get_number().str())));
    }
  } else {
    for (int i = 0; i < static_cast<int>(compiled.size()); i++) {
      reg_order.push_back(i);
    }
  }
  std::stable_sort(reg_order.begin(), reg_order.end(),
                   [&](int a, int b) { return compiled[a].priority < compiled[b].priority; });

  for (int mi : reg_order) {
    out.matchers.push_back(std::move(compiled[mi]));
  }

  // Anchor index (priority order preserved by construction).
  for (int i = 0; i < static_cast<int>(out.matchers.size()); i++) {
    const CompiledMatcher &m = out.matchers[i];
    if (m.unrunnable) {
      out.n_unrunnable++;
    }
    if (m.anchor_kind == AnchorKind::OpcodeSet) {
      for (std::uint32_t op : m.anchor_opcodes) {
        out.op_index[op].push_back(i);
      }
    } else if (m.anchor_kind == AnchorKind::BType) {
      for (const std::string &bt : m.anchor_btypes) {
        out.btype_index[bt].push_back(i);
      }
    }
  }
  return out;
}

}  // namespace mch
