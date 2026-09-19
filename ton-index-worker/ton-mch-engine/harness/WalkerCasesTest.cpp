// Covers walker selection, capture binding, and build rejection on small block trees.
#include "WalkerCasesTest.h"

#include "BlockTree.h"
#include "BuildDriver.h"
#include "BuildRuntime.h"
#include "ExprRuntime.h"
#include "HostRegistry.h"
#include "IrTables.h"
#include "Value.h"
#include "Walker.h"
#include "btypes_gen.h"

#include "td/utils/logging.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mch {
namespace {

constexpr std::uint32_t kFoo = 0xF00;
constexpr std::uint32_t kBar = 0xBA7;
constexpr std::uint32_t kBaz = 0xBA2;
constexpr std::uint32_t kQux = 0x0F1;

class EmptyLookupSource final : public LookupSource {
 public:
  Value fetch(const std::string &, const std::vector<Value> &) const override {
    return Value::null();
  }
};

class CaptureLog final : public td::LogInterface {
 public:
  void append(td::CSlice slice, int) override { text += slice.str(); }

  std::string text;
};

class ScopedLog final {
 public:
  explicit ScopedLog(td::LogInterface *replacement) : saved_(td::log_interface) {
    td::log_interface = replacement;
  }
  ~ScopedLog() { td::log_interface = saved_; }

 private:
  td::LogInterface *saved_;
};

Block *call(BlockArena &arena, std::uint32_t opcode, std::int64_t lt = 0) {
  Block *block = arena.make(mch::btype::kCallContract);
  block->opcode = opcode;
  block->min_lt = lt;
  return block;
}

Block *typed(BlockArena &arena, std::string btype, std::int64_t lt = 0) {
  Block *block = arena.make(std::move(btype));
  block->min_lt = lt;
  return block;
}

CompiledMatcher opcode_matcher(std::uint32_t opcode,
                               std::vector<std::string> slots = {}) {
  CompiledMatcher matcher;
  matcher.name = "walker_case";
  matcher.produces = {"walker_case"};
  matcher.anchor_kind = AnchorKind::OpcodeSet;
  matcher.anchor_opcodes.insert(opcode);
  matcher.slot_names = std::move(slots);
  matcher.cards.assign(matcher.slot_names.size(), "one");
  matcher.nodes.resize(1);
  matcher.nodes[0].kind = NodeKind::Contract;
  matcher.nodes[0].opcode = opcode;
  if (!matcher.slot_names.empty()) matcher.nodes[0].slot = 0;
  return matcher;
}

bool capture_is(const MatchResult &result, std::size_t slot, const Block *block) {
  return slot < result.captures.size() && !result.captures[slot].is_list &&
         result.captures[slot].vals == std::vector<Block *>{const_cast<Block *>(block)};
}

bool all_unparented(std::initializer_list<const Block *> blocks) {
  for (const Block *block : blocks) {
    if (block->parent != nullptr) return false;
  }
  return true;
}

Value consumed_value(const MatchResult &result) {
  std::vector<Value> values;
  for (Block *block : result.consumed) values.push_back(Value::make_block(block));
  return Value::make_list(std::move(values));
}

BuildOutcome run_build(const BuildFn &fn, Block *anchor, const MatchResult &result,
                       bool needs_lookups = false) {
  static const EmptyLookupSource source;
  return run_two_phase(fn, anchor, slots_from_captures(result.captures),
                       consumed_value(result), {}, source, needs_lookups);
}

Block *commit(BlockArena &arena, const CompiledMatcher &matcher,
              const MatchResult &result, BuildOutcome outcome) {
  if (outcome.is_rejected) return nullptr;
  Block *produced = arena.make(outcome.btype.empty() ? matcher.produces.front()
                                                     : outcome.btype);
  produced->data = std::move(outcome.data);
  produced->failed = outcome.failed;
  produced->broken = outcome.broken;
  return produced->merge_blocks(result.consumed) ? produced : nullptr;
}

EvalResult compare_dotfield(const WhereEnv &env, const std::string &field,
                            const Value &expected) {
  EvalResult value = rt_dotfield(env, field);
  return value.faulted ? value : rt_eq(value.value, expected);
}

EvalResult compare_entry_field(const WhereEnv &env, int entry_slot,
                               const std::string &candidate_field,
                               const std::string &entry_field) {
  if (entry_slot < 0 || static_cast<std::size_t>(entry_slot) >= env.n_slots) {
    return rt_fault("entry capture slot is unavailable");
  }
  EvalResult data = rt_access(env.slots[entry_slot], "data");
  if (data.faulted) return data;
  EvalResult entry = rt_access(data.value, entry_field);
  if (entry.faulted) return entry;
  return compare_dotfield(env, candidate_field, entry.value);
}

// Either opcode in the anchor set binds the same capture and reaches the build.
bool opcode_set_anchor_matches_both_heads() {
  CompiledMatcher matcher;
  matcher.name = "opcode_set_anchor_matches_both_heads";
  matcher.produces = {"fb"};
  matcher.anchor_kind = AnchorKind::OpcodeSet;
  matcher.anchor_opcodes = {kFoo, kBar};
  matcher.slot_names = {"x"};
  matcher.cards = {"one"};
  matcher.nodes.resize(1);
  matcher.nodes[0].kind = NodeKind::Any;
  matcher.nodes[0].slot = 0;

  BlockArena arena;
  for (std::uint32_t opcode : {kFoo, kBar}) {
    Block *anchor = call(arena, opcode);
    auto match = matcher_match(matcher, anchor, {});
    if (!match || !capture_is(*match, 0, anchor)) return false;
    BuildOutcome outcome = run_build(
        [](BuildEnv &env) {
          BuildOutcome out = BuildOutcome::accepted();
          const Block *captured = env.slots[0].block;
          out.data = Value::make_dict({
              {"oc", Value::make_int64(captured && captured->opcode
                                            ? static_cast<std::int64_t>(*captured->opcode)
                                            : -1)}});
          return out;
        },
        anchor, *match);
    const Value *oc = outcome.data.field("oc");
    EvalResult equal = oc != nullptr ? rt_eq(*oc, Value::make_int64(opcode))
                                     : rt_fault("missing opcode output");
    if (outcome.is_rejected || equal.faulted || !equal.value.boolean) {
      return false;
    }
  }
  Block *other = call(arena, 0x999);
  Block *non_call = typed(arena, mch::btype::kJettonTransfer);
  non_call->opcode = kFoo;
  return !matcher_test_self(matcher, other) && !matcher_match(matcher, other, {}) &&
         !matcher_test_self(matcher, non_call);
}

// A child where gate rejects a failing value without changing parent links.
bool where_expr_gates_child_match() {
  CompiledMatcher matcher = opcode_matcher(kFoo, {"x", "y"});
  matcher.nodes.resize(2);
  matcher.nodes[0].child = 1;
  matcher.nodes[1].kind = NodeKind::Contract;
  matcher.nodes[1].opcode = kBar;
  matcher.nodes[1].slot = 1;
  matcher.nodes[1].global_id = 1;
  matcher.nodes[1].has_where_expr = true;
  WhereTable wheres{{1, [](const WhereEnv &env) {
                       EvalResult value = rt_dotfield(env, "value");
                       return value.faulted ? value
                                            : rt_gt(value.value, Value::make_int64(5));
                     }}};

  BlockArena arena;
  Block *pass = call(arena, kFoo);
  Block *pass_child = call(arena, kBar);
  pass_child->data = Value::make_dict({{"value", Value::make_int64(10)}});
  pass->connect(pass_child);
  Block *fail = call(arena, kFoo);
  Block *fail_child = call(arena, kBar);
  fail_child->data = Value::make_dict({{"value", Value::make_int64(3)}});
  fail->connect(fail_child);
  auto matched = matcher_match(matcher, pass, wheres);
  return matched && capture_is(*matched, 1, pass_child) &&
         !matcher_match(matcher, fail, wheres) && all_unparented({fail, fail_child});
}

// A where expression uses the entry capture to select and consume the matching child.
bool where_expr_selects_child_using_entry_capture() {
  CompiledMatcher matcher = opcode_matcher(kFoo, {"x", "y"});
  matcher.nodes.resize(2);
  matcher.nodes[0].child = 1;
  matcher.nodes[1].kind = NodeKind::Contract;
  matcher.nodes[1].opcode = kBar;
  matcher.nodes[1].slot = 1;
  matcher.nodes[1].global_id = 1;
  matcher.nodes[1].has_where_expr = true;
  WhereTable wheres{{1, [](const WhereEnv &env) {
                       return compare_entry_field(env, 0, "destination", "owner");
                     }}};

  BlockArena arena;
  Block *anchor = call(arena, kFoo, 10);
  anchor->data = Value::make_dict({{"owner", Value::make_str("seller")}});
  Block *wrong = call(arena, kBar, 15);
  wrong->data = Value::make_dict({{"destination", Value::make_str("fee")}});
  Block *payout = call(arena, kBar, 20);
  payout->data = Value::make_dict({{"destination", Value::make_str("seller")}});
  anchor->connect(wrong);
  anchor->connect(payout);
  auto matched = matcher_match(matcher, anchor, wheres);

  Block *rejected = call(arena, kFoo, 30);
  rejected->data = Value::make_dict({{"owner", Value::make_str("seller")}});
  Block *only_wrong = call(arena, kBar, 40);
  only_wrong->data = Value::make_dict({{"destination", Value::make_str("fee")}});
  rejected->connect(only_wrong);
  return matched && capture_is(*matched, 1, payout) &&
         matched->consumed == std::vector<Block *>({anchor, payout}) &&
         !matcher_match(matcher, rejected, wheres);
}

// A peek uses the entry capture to select a sibling without consuming it or its parent.
bool where_expr_selects_peeked_sibling_using_entry_capture() {
  CompiledMatcher matcher = opcode_matcher(kFoo, {"x", "sib"});
  matcher.nodes.resize(3);
  matcher.nodes[0].parent = 1;
  matcher.nodes[1].kind = NodeKind::Contract;
  matcher.nodes[1].opcode = kBar;
  matcher.nodes[1].peek = true;
  matcher.nodes[1].children = {2};
  matcher.nodes[2].kind = NodeKind::Contract;
  matcher.nodes[2].opcode = kBaz;
  matcher.nodes[2].slot = 1;
  matcher.nodes[2].peek = true;
  matcher.nodes[2].global_id = 2;
  matcher.nodes[2].has_where_expr = true;
  WhereTable wheres{{2, [](const WhereEnv &env) {
                       return compare_entry_field(env, 0, "destination", "owner");
                     }}};

  BlockArena arena;
  Block *parent = call(arena, kBar, 5);
  Block *anchor = call(arena, kFoo, 10);
  anchor->data = Value::make_dict({{"owner", Value::make_str("seller")}});
  Block *wrong = call(arena, kBaz, 15);
  wrong->data = Value::make_dict({{"destination", Value::make_str("fee")}});
  Block *payout = call(arena, kBaz, 20);
  payout->data = Value::make_dict({{"destination", Value::make_str("seller")}});
  parent->connect(anchor);
  parent->connect(wrong);
  parent->connect(payout);
  auto matched = matcher_match(matcher, anchor, wheres);

  Block *parent2 = call(arena, kBar, 25);
  Block *anchor2 = call(arena, kFoo, 30);
  anchor2->data = Value::make_dict({{"owner", Value::make_str("seller")}});
  Block *wrong2 = call(arena, kBaz, 40);
  wrong2->data = Value::make_dict({{"destination", Value::make_str("fee")}});
  parent2->connect(anchor2);
  parent2->connect(wrong2);
  return matched && capture_is(*matched, 1, payout) &&
         matched->consumed == std::vector<Block *>{anchor} &&
         !matcher_match(matcher, anchor2, wheres);
}

// A data-field gate accepts false and rejects true, missing fields, and null data.
bool where_expr_data_check_replaces_generic_matcher() {
  CompiledMatcher matcher = opcode_matcher(kFoo);
  matcher.nodes.resize(2);
  matcher.nodes[0].child = 1;
  matcher.nodes[1].kind = NodeKind::BlockType;
  matcher.nodes[1].btype = mch::btype::kJettonTransfer;
  matcher.nodes[1].global_id = 1;
  matcher.nodes[1].has_where_expr = true;
  WhereTable wheres{{1, [](const WhereEnv &env) {
                       return compare_dotfield(env, "has_internal_transfer",
                                               Value::make_bool(false));
                     }}};
  auto matches = [&](std::optional<bool> value, bool null_data = false) {
    BlockArena arena;
    Block *anchor = call(arena, kFoo);
    Block *transfer = typed(arena, mch::btype::kJettonTransfer);
    if (!null_data) {
      Value::Fields fields;
      if (value) fields.emplace_back("has_internal_transfer", Value::make_bool(*value));
      transfer->data = Value::make_dict(std::move(fields));
    }
    anchor->connect(transfer);
    return matcher_match(matcher, anchor, wheres).has_value();
  };
  return matches(false) && !matches(true) && !matches(std::nullopt) &&
         !matches(std::nullopt, true);
}

// An anchor where gate rejects mismatched or missing data despite a matching child.
bool where_expr_on_anchor_node() {
  CompiledMatcher matcher = opcode_matcher(kFoo, {"x", "y"});
  matcher.nodes.resize(2);
  matcher.nodes[0].global_id = 0;
  matcher.nodes[0].has_where_expr = true;
  matcher.nodes[0].child = 1;
  matcher.nodes[1].kind = NodeKind::Contract;
  matcher.nodes[1].opcode = kBar;
  matcher.nodes[1].slot = 1;
  WhereTable wheres{{0, [](const WhereEnv &env) {
                       return compare_dotfield(env, "kind", Value::make_str("swap"));
                     }}};
  auto matches = [&](std::optional<std::string> kind) {
    BlockArena arena;
    Block *anchor = call(arena, kFoo);
    if (kind) anchor->data = Value::make_dict({{"kind", Value::make_str(*kind)}});
    anchor->connect(call(arena, kBar));
    return matcher_match(matcher, anchor, wheres).has_value();
  };
  return matches("swap") && !matches("transfer") && !matches(std::nullopt);
}

// A named predicate gates the node before binding its capture.
bool named_where_predicate_gates_then_binds_capture() {
  CompiledMatcher matcher = opcode_matcher(kFoo, {"a"});
  matcher.nodes[0].where_name = "sale_contract_deploy";
  matcher.ref_preds.insert("sale_contract_deploy");
  BlockArena arena;
  Block *plain = call(arena, kFoo);

  Transaction tx;
  tx.orig_status = "uninit";
  tx.end_status = "active";
  EventNode node;
  node.tx = &tx;
  Block *special = call(arena, kFoo);
  special->event_nodes.push_back(&node);
  auto matched = matcher_match(matcher, special, {});
  return match_skip_reason(matcher).empty() && !matcher_match(matcher, plain, {}) &&
         matched && capture_is(*matched, 0, special);
}

// A block-type anchor binds the matching block to its capture.
bool btype_anchor_capture_binding() {
  CompiledMatcher matcher;
  matcher.name = "btype_anchor_capture_binding";
  matcher.anchor_kind = AnchorKind::BType;
  matcher.anchor_btypes = {mch::btype::kJettonTransfer};
  matcher.slot_names = {"j"};
  matcher.cards = {"one"};
  matcher.nodes.resize(1);
  matcher.nodes[0].kind = NodeKind::BlockType;
  matcher.nodes[0].btype = mch::btype::kJettonTransfer;
  matcher.nodes[0].slot = 0;
  BlockArena arena;
  Block *tree = typed(arena, mch::btype::kJettonTransfer);
  auto matched = matcher_match(matcher, tree, {});
  return matched && capture_is(*matched, 0, tree);
}

// An opcode anchor and block-type child bind their respective captures.
bool mixed_op_then_btype_chain_capture_binding() {
  CompiledMatcher matcher = opcode_matcher(kFoo, {"a", "j"});
  matcher.nodes.resize(2);
  matcher.nodes[0].child = 1;
  matcher.nodes[1].kind = NodeKind::BlockType;
  matcher.nodes[1].btype = mch::btype::kJettonTransfer;
  matcher.nodes[1].slot = 1;
  BlockArena arena;
  Block *anchor = call(arena, kFoo);
  Block *transfer = typed(arena, mch::btype::kJettonTransfer);
  anchor->connect(transfer);
  auto matched = matcher_match(matcher, anchor, {});
  return matched && capture_is(*matched, 0, anchor) && capture_is(*matched, 1, transfer);
}

// A failing second parent hop rejects the whole chain without changing parent links.
bool two_hop_parent_chain_partial_match_rejected() {
  CompiledMatcher matcher = opcode_matcher(kFoo, {"a", "b", "c"});
  matcher.nodes.resize(3);
  matcher.nodes[0].parent = 1;
  matcher.nodes[1].kind = NodeKind::Contract;
  matcher.nodes[1].opcode = kBar;
  matcher.nodes[1].slot = 1;
  matcher.nodes[1].parent = 2;
  matcher.nodes[2].kind = NodeKind::Contract;
  matcher.nodes[2].opcode = kBaz;
  matcher.nodes[2].slot = 2;
  BlockArena arena;
  Block *wrong_root = call(arena, kQux);
  Block *middle = call(arena, kBar);
  Block *anchor = call(arena, kFoo);
  wrong_root->connect(middle);
  middle->connect(anchor);
  return !matcher_match(matcher, anchor, {}) && all_unparented({wrong_root, middle, anchor});
}

// An opcode-set anchor still requires its child before captures form a match.
bool opcode_set_anchor_required_child_fails_without_it() {
  CompiledMatcher matcher;
  matcher.name = "opcode_set_anchor_required_child_fails_without_it";
  matcher.anchor_kind = AnchorKind::OpcodeSet;
  matcher.anchor_opcodes = {kFoo, kBar};
  matcher.slot_names = {"x", "c"};
  matcher.cards = {"one", "one"};
  matcher.nodes.resize(2);
  matcher.nodes[0].kind = NodeKind::Any;
  matcher.nodes[0].slot = 0;
  matcher.nodes[0].child = 1;
  matcher.nodes[1].kind = NodeKind::Contract;
  matcher.nodes[1].opcode = kBaz;
  matcher.nodes[1].slot = 1;
  BlockArena arena;
  Block *anchor = call(arena, kFoo);
  Block *child = call(arena, kBaz);
  anchor->connect(child);
  auto matched = matcher_match(matcher, anchor, {});
  Block *bare = call(arena, kBar);
  return matched && capture_is(*matched, 0, anchor) && capture_is(*matched, 1, child) &&
         !matcher_match(matcher, bare, {});
}

// A host function's null result supports rejection without consuming the anchor.
bool fn_returning_null_supports_reject() {
  CompiledMatcher matcher = opcode_matcher(kFoo, {"x"});
  BlockArena arena;
  Block *anchor = call(arena, kFoo);
  auto matched = matcher_match(matcher, anchor, {});
  if (!matched || find_hostfn("tgbtc_mint_data") == nullptr) return false;
  BuildOutcome outcome = run_build(
      [](BuildEnv &env) {
        EvalResult result = rt_call_hostfn(env, "tgbtc_mint_data",
                                           {Value::null(), Value::null(), Value::null()});
        if (result.faulted) return BuildOutcome::rejected(result.message);
        return result.value.is_null() ? BuildOutcome::rejected("reject when")
                                      : BuildOutcome::accepted();
      },
      anchor, *matched, true);
  return outcome.is_rejected && anchor->parent == nullptr;
}

EvalResult throwing_host(BuildEnv &, const std::vector<Value> &) {
  throw std::runtime_error("boom");
}

// A host exception becomes a logged rejection and leaves the anchor unparented.
bool fn_exception_rejects_match() {
  static const HostFnEntry entry{throwing_host, 1};
  CompiledMatcher matcher = opcode_matcher(kFoo, {"x"});
  BlockArena arena;
  Block *anchor = call(arena, kFoo);
  auto matched = matcher_match(matcher, anchor, {});
  if (!matched) return false;
  CaptureLog log;
  ScopedLog scoped_log(&log);
  BuildOutcome outcome = run_build(
      [](BuildEnv &env) {
        EvalResult result = rt_call_hostfn(env, "throwing_walker_test_host", &entry,
                                           {Value::make_block(env.anchor)});
        return result.faulted ? BuildOutcome::rejected(result.message)
                              : BuildOutcome::accepted();
      },
      anchor, *matched, true);
  return outcome.is_rejected && outcome.reject_reason == "boom" && anchor->parent == nullptr &&
         log.text.find("[mch-reject]") != std::string::npos &&
         log.text.find("reason=boom") != std::string::npos;
}

// One child cannot satisfy two required children-set members.
bool r5_required_item_unmatched_fails() {
  CompiledMatcher matcher = opcode_matcher(kFoo, {"x", "a", "b"});
  matcher.nodes.resize(3);
  matcher.nodes[0].children = {1, 2};
  for (int i : {1, 2}) {
    matcher.nodes[i].kind = NodeKind::Contract;
    matcher.nodes[i].opcode = kBar;
    matcher.nodes[i].slot = i;
  }
  BlockArena arena;
  Block *anchor = call(arena, kFoo);
  Block *only = call(arena, kBar);
  anchor->connect(only);
  return !matcher_match(matcher, anchor, {}) && all_unparented({anchor, only});
}

// Rejection leaves auxiliary blocks unattached and permits later reuse on the same tree.
bool r7_rejection_no_aux_and_matcher_reusable() {
  CompiledMatcher matcher = opcode_matcher(kFoo, {"x", "y"});
  matcher.nodes.resize(2);
  matcher.nodes[0].child = 1;
  matcher.nodes[1].kind = NodeKind::Contract;
  matcher.nodes[1].opcode = kBar;
  matcher.nodes[1].slot = 1;
  BlockArena arena;
  Block *anchor = call(arena, kFoo, 10);
  Block *child = call(arena, kBar, 20);
  Block *excess = call(arena, kExcessOpcode, 30);
  anchor->connect(child);
  anchor->connect(excess);
  BuildFn reject = [](BuildEnv &) { return BuildOutcome::rejected("reject when"); };
  for (int i = 0; i < 2; ++i) {
    auto matched = matcher_match(matcher, anchor, {});
    if (!matched || !run_build(reject, anchor, *matched).is_rejected ||
        !all_unparented({anchor, child, excess})) {
      return false;
    }
  }
  auto matched = matcher_match(matcher, anchor, {});
  if (!matched || matched->consumed != std::vector<Block *>({anchor, child, excess})) {
    return false;
  }
  Block *produced = commit(arena, matcher, *matched, BuildOutcome::accepted());
  return produced && child->parent == produced && excess->parent == produced;
}

// Unconditional rejection leaves both the anchor and its excess child unparented.
bool r7_reject_when_returns_empty_no_aux() {
  CompiledMatcher matcher = opcode_matcher(kFoo, {"x"});
  BlockArena arena;
  Block *anchor = call(arena, kFoo);
  Block *excess = call(arena, kExcessOpcode);
  anchor->connect(excess);
  auto matched = matcher_match(matcher, anchor, {});
  if (!matched) return false;
  BuildOutcome outcome = run_build(
      [](BuildEnv &) { return BuildOutcome::rejected("reject when"); }, anchor, *matched);
  return outcome.is_rejected && all_unparented({anchor, excess});
}

// Exclusive matching requires exactly one head after named and inline where gates.
bool exclusive_counts_heads_after_where() {
  CompiledMatcher matcher = opcode_matcher(kFoo, {"x", "a", "b"});
  matcher.nodes.resize(4);
  matcher.nodes[0].child = 1;
  matcher.nodes[1].kind = NodeKind::Or;
  matcher.nodes[1].exclusive = true;
  matcher.nodes[1].branches = {2, 3};
  matcher.nodes[2].kind = NodeKind::Contract;
  matcher.nodes[2].opcode = kBar;
  matcher.nodes[2].slot = 1;
  matcher.nodes[2].where_name = "sale_contract_deploy";
  matcher.nodes[3].kind = NodeKind::Contract;
  matcher.nodes[3].opcode = kBar;
  matcher.nodes[3].slot = 2;
  matcher.nodes[3].global_id = 3;
  matcher.nodes[3].has_where_expr = true;
  matcher.ref_preds.insert("sale_contract_deploy");
  if (!match_skip_reason(matcher).empty()) return false;
  for (const auto &[left, right] :
       std::vector<std::pair<bool, bool>>{{false, false}, {false, true}, {true, true}}) {
    BlockArena arena;
    Block *anchor = call(arena, kFoo);
    Block *child = call(arena, kBar);
    Transaction tx;
    tx.orig_status = left ? "uninit" : "active";
    tx.end_status = "active";
    EventNode node;
    node.tx = &tx;
    child->event_nodes.push_back(&node);
    anchor->connect(child);
    WhereTable wheres{{3, [right](const WhereEnv &) {
                         return rt_ok(Value::make_bool(right));
                       }}};
    auto matched = matcher_match(matcher, anchor, wheres);
    if (left == right) {
      if (matched) return false;
    } else if (!matched || !capture_is(*matched, 1, nullptr) ||
               !capture_is(*matched, 2, child)) {
      return false;
    }
  }
  return true;
}

// Zero matching heads reject both exclusive and non-exclusive alternatives.
bool r9_zero_heads_fail() {
  CompiledMatcher matcher = opcode_matcher(kFoo);
  matcher.nodes.resize(4);
  matcher.nodes[0].child = 1;
  matcher.nodes[1].kind = NodeKind::Or;
  matcher.nodes[1].branches = {2, 3};
  matcher.nodes[2].kind = NodeKind::Contract;
  matcher.nodes[2].opcode = kBar;
  matcher.nodes[3].kind = NodeKind::Contract;
  matcher.nodes[3].opcode = kBaz;
  for (bool exclusive : {false, true}) {
    matcher.nodes[1].exclusive = exclusive;
    BlockArena arena;
    Block *anchor = call(arena, kFoo);
    anchor->connect(call(arena, kQux));
    if (matcher_match(matcher, anchor, {})) return false;
  }
  return true;
}

// A failing anchor predicate prevents the build from running.
bool r10_pred_anchor_gates_try_build() {
  CompiledMatcher matcher;
  matcher.name = "r10_pred_anchor_gates_try_build";
  matcher.anchor_kind = AnchorKind::Pred;
  matcher.anchor_pred = "sale_contract_deploy";
  matcher.ref_preds.insert("sale_contract_deploy");
  matcher.nodes.resize(1);
  matcher.nodes[0].kind = NodeKind::Any;
  BlockArena arena;
  Block *unmarked = call(arena, kFoo);
  bool build_called = false;
  auto matched = matcher_match(matcher, unmarked, {});
  if (matched) {
    build_called = true;
    run_build([](BuildEnv &) { return BuildOutcome::accepted(); }, unmarked, *matched);
  }
  return match_skip_reason(matcher).empty() && !matcher_test_self(matcher, unmarked) &&
         !matched && !build_called;
}

// An optional branch does not make a required alternative succeed without a head.
bool mch_maybe_branch_inside_alternative_is_inert() {
  CompiledMatcher matcher = opcode_matcher(1, {"a"});
  matcher.nodes.resize(4);
  matcher.nodes[0].child = 1;
  matcher.nodes[1].kind = NodeKind::Or;
  matcher.nodes[1].branches = {2, 3};
  matcher.nodes[2].kind = NodeKind::Contract;
  matcher.nodes[2].opcode = 2;
  matcher.nodes[2].optional = true;
  matcher.nodes[3].kind = NodeKind::Contract;
  matcher.nodes[3].opcode = 3;
  BlockArena arena;
  Block *neither = call(arena, 1);
  neither->connect(call(arena, 9));
  Block *with_y = call(arena, 1);
  with_y->connect(call(arena, 3));
  auto matched = matcher_match(matcher, with_y, {});
  return !matcher_match(matcher, neither, {}) && matched &&
         capture_is(*matched, 0, with_y);
}

// Rejection leaves the anchor available for the next matcher to consume.
bool rejecting_matcher_lets_next_matcher_consume_same_anchor() {
  CompiledMatcher first = opcode_matcher(kFoo, {"x"});
  first.name = "rejects_first";
  first.produces = {"fb"};
  CompiledMatcher second = first;
  second.name = "accepts_second";
  BlockArena arena;
  Block *anchor = call(arena, kFoo);
  auto first_match = matcher_match(first, anchor, {});
  if (!first_match) return false;
  BuildOutcome rejected = run_build(
      [](BuildEnv &) { return BuildOutcome::rejected("reject when"); }, anchor, *first_match);
  if (!rejected.is_rejected || anchor->parent != nullptr) return false;
  auto second_match = matcher_match(second, anchor, {});
  if (!second_match) return false;
  BuildOutcome accepted = BuildOutcome::accepted();
  accepted.data = Value::make_dict({{"ok", Value::make_bool(true)}});
  Block *produced = commit(arena, second, *second_match, std::move(accepted));
  const Value *ok = produced != nullptr ? produced->data.field("ok") : nullptr;
  return produced && ok && ok->t == VType::Bool && ok->boolean && anchor->parent == produced;
}

bool clean_rejection_with_aux(const std::string &reason) {
  CompiledMatcher matcher = opcode_matcher(kFoo, {"x"});
  BlockArena arena;
  Block *anchor = call(arena, kFoo);
  Block *excess = call(arena, kExcessOpcode);
  Block *bounce = call(arena, kBounceOpcode);
  anchor->connect(excess);
  anchor->connect(bounce);
  auto matched = matcher_match(matcher, anchor, {});
  if (!matched) return false;
  BuildOutcome outcome = run_build(
      [reason](BuildEnv &) { return BuildOutcome::rejected(reason); }, anchor, *matched);
  return outcome.is_rejected && all_unparented({anchor, excess, bounce}) &&
         anchor->next_blocks == std::vector<Block *>({excess, bounce});
}

// Build rejection preserves excess and bounce child links without attaching parents.
bool rejection_with_excess_and_bounce_children_stays_clean() {
  return clean_rejection_with_aux("rejects");
}

// A reject-when outcome leaves auxiliary children linked and unparented.
bool declarative_reject_when_stays_clean_with_aux_children() {
  return clean_rejection_with_aux("reject when");
}

struct Case {
  const char *name;
  bool (*fn)();
};

}  // namespace

int run_walker_cases_test() {
  const Case cases[] = {
      {"opcode_set_anchor_matches_both_heads", opcode_set_anchor_matches_both_heads},
      {"where_expr_gates_child_match", where_expr_gates_child_match},
      {"where_expr_selects_child_using_entry_capture",
       where_expr_selects_child_using_entry_capture},
      {"where_expr_selects_peeked_sibling_using_entry_capture",
       where_expr_selects_peeked_sibling_using_entry_capture},
      {"where_expr_data_check_replaces_generic_matcher",
       where_expr_data_check_replaces_generic_matcher},
      {"where_expr_on_anchor_node", where_expr_on_anchor_node},
      {"named_where_predicate_gates_then_binds_capture",
       named_where_predicate_gates_then_binds_capture},
      {"btype_anchor_capture_binding", btype_anchor_capture_binding},
      {"mixed_op_then_btype_chain_capture_binding",
       mixed_op_then_btype_chain_capture_binding},
      {"two_hop_parent_chain_partial_match_rejected",
       two_hop_parent_chain_partial_match_rejected},
      {"opcode_set_anchor_required_child_fails_without_it",
       opcode_set_anchor_required_child_fails_without_it},
      {"fn_returning_null_supports_reject", fn_returning_null_supports_reject},
      {"fn_exception_rejects_match", fn_exception_rejects_match},
      {"r5_required_item_unmatched_fails", r5_required_item_unmatched_fails},
      {"r7_rejection_no_aux_and_matcher_reusable",
       r7_rejection_no_aux_and_matcher_reusable},
      {"r7_reject_when_returns_empty_no_aux", r7_reject_when_returns_empty_no_aux},
      {"exclusive_counts_heads_after_where", exclusive_counts_heads_after_where},
      {"r9_zero_heads_fail", r9_zero_heads_fail},
      {"r10_pred_anchor_gates_try_build", r10_pred_anchor_gates_try_build},
      {"mch_maybe_branch_inside_alternative_is_inert",
       mch_maybe_branch_inside_alternative_is_inert},
      {"rejecting_matcher_lets_next_matcher_consume_same_anchor",
       rejecting_matcher_lets_next_matcher_consume_same_anchor},
      {"rejection_with_excess_and_bounce_children_stays_clean",
       rejection_with_excess_and_bounce_children_stays_clean},
      {"declarative_reject_when_stays_clean_with_aux_children",
       declarative_reject_when_stays_clean_with_aux_children},
  };

  int failures = 0;
  for (const Case &test : cases) {
    bool passed = false;
    try {
      passed = test.fn();
    } catch (const std::exception &e) {
      std::fprintf(stderr, "walker case %s threw: %s\n", test.name, e.what());
    } catch (...) {
      std::fprintf(stderr, "walker case %s threw an unknown exception\n", test.name);
    }
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", test.name);
    failures += passed ? 0 : 1;
  }
  std::printf("WALKER-CASES %s (%zu cases)\n", failures == 0 ? "ALL PASS" : "FAILURES",
              sizeof(cases) / sizeof(cases[0]));
  return failures == 0 ? 0 : 1;
}

}  // namespace mch
