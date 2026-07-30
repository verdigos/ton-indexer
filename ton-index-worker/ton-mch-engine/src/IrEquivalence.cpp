#include "IrEquivalence.h"

#include "GenMatchers.h"
#include "fixtures/IrLoader.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace mch {

namespace {

std::string show(bool v) { return v ? "true" : "false"; }
std::string show(int v) { return std::to_string(v); }
std::string show(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08x", v);
  return buf;
}
std::string show(const std::string &v) { return "'" + v + "'"; }
std::string show(NodeKind k) {
  switch (k) {
    case NodeKind::Contract: return "contract";
    case NodeKind::BlockType: return "block_type";
    case NodeKind::Pred: return "pred";
    case NodeKind::Any: return "any";
    case NodeKind::Or: return "or";
    case NodeKind::Recursive: return "recursive";
  }
  return "?";
}
std::string show(AnchorKind k) {
  switch (k) {
    case AnchorKind::OpcodeSet: return "opcode_set";
    case AnchorKind::BType: return "btype";
    case AnchorKind::Pred: return "pred";
    case AnchorKind::Mixed: return "mixed";
  }
  return "?";
}
std::string show(RecStrategy s) { return s == RecStrategy::Cyclic ? "cyclic" : "frontier"; }

// Containers render in SORTED order so an unordered_set's message is stable.
template <typename C>
std::string show_range(const C &c) {
  std::vector<std::string> parts;
  for (const auto &e : c) {
    parts.push_back(show(e));
  }
  std::sort(parts.begin(), parts.end());
  std::string out = "[";
  for (std::size_t i = 0; i < parts.size(); i++) {
    if (i) out += ",";
    out += parts[i];
  }
  return out + "]";
}
std::string show(const std::vector<int> &v) { return show_range(v); }
std::string show(const std::vector<std::string> &v) { return show_range(v); }
std::string show(const std::set<int> &v) { return show_range(v); }
std::string show(const std::set<std::string> &v) { return show_range(v); }
std::string show(const std::unordered_set<std::string> &v) { return show_range(v); }
std::string show(const std::unordered_set<std::uint32_t> &v) { return show_range(v); }
std::string show(const std::map<int, std::set<int>> &m) {
  std::string out = "{";
  bool first = true;
  for (const auto &kv : m) {
    if (!first) out += ",";
    first = false;
    out += std::to_string(kv.first) + "=>" + show(kv.second);
  }
  return out + "}";
}

// Records the FIRST divergence and then short-circuits, so the message names one
// field of one matcher rather than a wall of consequences.
class Diff {
 public:
  bool failed() const { return !msg_.empty(); }
  const std::string &msg() const { return msg_; }

  template <typename T>
  void eq(const std::string &path, const T &generated, const T &loaded) {
    if (failed() || generated == loaded) {
      return;
    }
    msg_ = path + ": generated=" + show(generated) + " loaded=" + show(loaded);
  }

 private:
  std::string msg_;
};

void compare_node(Diff &d, const std::string &path, const CompiledNode &g, const CompiledNode &l) {
  d.eq(path + ".kind", g.kind, l.kind);
  d.eq(path + ".global_id", g.global_id, l.global_id);
  d.eq(path + ".opcode", g.opcode, l.opcode);
  d.eq(path + ".has_opcode", g.has_opcode, l.has_opcode);
  d.eq(path + ".btype", g.btype, l.btype);
  d.eq(path + ".pred_name", g.pred_name, l.pred_name);
  d.eq(path + ".where_name", g.where_name, l.where_name);
  d.eq(path + ".has_where_expr", g.has_where_expr, l.has_where_expr);
  d.eq(path + ".slot", g.slot, l.slot);
  d.eq(path + ".optional", g.optional, l.optional);
  d.eq(path + ".child", g.child, l.child);
  d.eq(path + ".children", g.children, l.children);
  d.eq(path + ".parent", g.parent, l.parent);
  d.eq(path + ".branches", g.branches, l.branches);
  d.eq(path + ".step", g.step, l.step);
  d.eq(path + ".exit", g.exit, l.exit);
  d.eq(path + ".exclusive", g.exclusive, l.exclusive);
  d.eq(path + ".strategy", g.strategy, l.strategy);
}

// Mixed-anchor branches compare as one rendered string: they are ORDERED
// (source order decides which branch's `where` runs), so a set/vector compare
// would either lose the order or need a per-field element diff.
std::string show_branches(const std::vector<CompiledMatcher::AnchorBranch> &brs) {
  std::string out = "[";
  for (std::size_t i = 0; i < brs.size(); i++) {
    if (i) out += ",";
    out += brs[i].is_op ? "op:" + std::to_string(brs[i].opcode) : "btype:" + brs[i].btype;
    if (!brs[i].where.empty()) {
      out += "/" + brs[i].where;
    }
  }
  return out + "]";
}

void compare_matcher(Diff &d, const std::string &path, const CompiledMatcher &g,
                     const CompiledMatcher &l) {
  d.eq(path + ".name", g.name, l.name);
  d.eq(path + ".artifact_index", g.artifact_index, l.artifact_index);
  d.eq(path + ".has_build_program", g.has_build_program, l.has_build_program);
  d.eq(path + ".produces", g.produces, l.produces);
  d.eq(path + ".anchor_kind", g.anchor_kind, l.anchor_kind);
  d.eq(path + ".anchor_opcodes", g.anchor_opcodes, l.anchor_opcodes);
  d.eq(path + ".anchor_btypes", g.anchor_btypes, l.anchor_btypes);
  d.eq(path + ".anchor_pred", g.anchor_pred, l.anchor_pred);
  d.eq(path + ".anchor_branches", show_branches(g.anchor_branches),
       show_branches(l.anchor_branches));
  d.eq(path + ".slot_names", g.slot_names, l.slot_names);
  d.eq(path + ".cards", g.cards, l.cards);
  d.eq(path + ".step_slots", g.step_slots, l.step_slots);
  d.eq(path + ".exit_slots", g.exit_slots, l.exit_slots);
  d.eq(path + ".owned_slots", g.owned_slots, l.owned_slots);
  d.eq(path + ".include_excess", g.include_excess, l.include_excess);
  d.eq(path + ".include_bounces", g.include_bounces, l.include_bounces);
  d.eq(path + ".priority", g.priority, l.priority);
  d.eq(path + ".ref_preds", g.ref_preds, l.ref_preds);
  d.eq(path + ".ref_builders", g.ref_builders, l.ref_builders);
  d.eq(path + ".ref_shapers", g.ref_shapers, l.ref_shapers);
  d.eq(path + ".ref_msgtypes", g.ref_msgtypes, l.ref_msgtypes);
  d.eq(path + ".ref_lookups", g.ref_lookups, l.ref_lookups);
  d.eq(path + ".ref_fns", g.ref_fns, l.ref_fns);
  d.eq(path + ".nodes.size", static_cast<int>(g.nodes.size()), static_cast<int>(l.nodes.size()));
  if (d.failed()) {
    return;
  }
  for (std::size_t i = 0; i < g.nodes.size(); i++) {
    compare_node(d, path + ".nodes[" + std::to_string(i) + "]", g.nodes[i], l.nodes[i]);
    if (d.failed()) {
      return;
    }
  }
}

}  // namespace

int run_ir_equivalence(const std::string &artifact_path) {
  auto r_ir = load_ir(artifact_path);
  if (r_ir.is_error()) {
    std::fprintf(stderr, "failed to load IR artifact: %s\n", r_ir.error().message().c_str());
    return 2;
  }
  LoadedIr loaded = r_ir.move_as_ok();
  const std::vector<CompiledMatcher> &generated = gen_matchers_ir();

  // Same document on both sides, or the comparison means nothing. This is the
  // check that keeps the gate honest when the table is generated from a testbed
  // artifact instead of the committed one.
  if (loaded.source_sha256 != gen_matchers_ir_source_sha()) {
    std::fprintf(stderr,
                 "artifact mismatch: %s has sha %s, the compiled table was generated from %s — "
                 "run this gate against the document the build compiled in\n",
                 artifact_path.c_str(), loaded.source_sha256.c_str(),
                 gen_matchers_ir_source_sha());
    return 1;
  }

  Diff d;
  d.eq("matchers.size", static_cast<int>(generated.size()), static_cast<int>(loaded.matchers.size()));
  for (std::size_t i = 0; i < generated.size() && !d.failed(); i++) {
    // Index the message by matcher NAME: a table-order divergence shows up as a
    // name mismatch on the first differing slot, which reads better than "[17]".
    compare_matcher(d, "matcher[" + std::to_string(i) + " " + loaded.matchers[i].name + "]",
                    generated[i], loaded.matchers[i]);
  }
  if (d.failed()) {
    std::fprintf(stderr, "mch_ir_equivalence: FAIL %s\n", d.msg().c_str());
    return 1;
  }

  std::size_t nodes = 0;
  for (const CompiledMatcher &m : generated) {
    nodes += m.nodes.size();
  }
  std::fprintf(stderr,
               "mch_ir_equivalence: ALL PASS (%zu matchers, %zu nodes identical; artifact sha %s)\n",
               generated.size(), nodes, gen_matchers_ir_source_sha());
  return 0;
}

}  // namespace mch
