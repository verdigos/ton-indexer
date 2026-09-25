#include "ClassifyCore.h"
#include "ActionPayload.h"

#include "BuildDriver.h"
#include "EnginePrep.h"
#include "ExprRuntime.h"
#include "GenBuilds.h"
#include "GenMatchers.h"
#include "GenWheres.h"
#include "GhostExternal.h"
#include "HostRegistry.h"
#include "TieredLookupSource.h"
#include "SchemaTraceLoader.h"
#include "Walker.h"
#include "btypes_gen.h"

#include "host/HostCommon.h"
#include "host/HostImpls.h"

#include "vm/excno.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_set>
#include <variant>

namespace mch {

bool outcomes_allow_replacement(const BlockClassification &bc) {
  return std::all_of(bc.traces.begin(), bc.traces.end(), [](const TraceClassificationResult &trace) {
    return should_persist_outcome(trace.outcome) || trace.outcome == TraceOutcome::skipped_capped ||
           trace.outcome == TraceOutcome::skipped_lone_tick_tock || trace.outcome == TraceOutcome::skipped_broken;
  });
}

static std::string exception_message_with_prefixes(std::exception_ptr error,
                                                   const char *vm_prefix,
                                                   const char *exception_prefix) {
  if (!error) return "unknown exception";
  try {
    std::rethrow_exception(error);
  } catch (const vm::VmError &e) {
    return std::string(vm_prefix) + e.get_msg();
  } catch (const vm::VmNoGas &e) {
    return std::string(vm_prefix) + e.get_msg();
  } catch (const vm::VmVirtError &e) {
    return std::string(vm_prefix) + e.get_msg();
  } catch (const std::exception &e) {
    return std::string(exception_prefix) + e.what();
  } catch (...) {
    return "unknown exception";
  }
}

std::string exception_message(std::exception_ptr error) {
  return exception_message_with_prefixes(error, "", "");
}

std::string exception_message(std::exception_ptr error, bool prefix_exception) {
  return exception_message_with_prefixes(error, "vm exception: ",
                                         prefix_exception ? "exception: " : "");
}

namespace {

Block *wrap_root(BlockArena &arena, Block *root) {
  Block *wrapper = arena.make(mch::btype::kRoot);
  wrapper->connect(root);
  return wrapper;
}

std::pair<std::string, FailureCategory> exception_failure(std::exception_ptr error,
                                                          bool prefix_exception) {
  try {
    if (error) std::rethrow_exception(error);
  } catch (const LookupNotConverged &e) {
    return {e.what(), FailureCategory::lookup_infra_fail};
  } catch (...) {
  }
  return {exception_message(error, prefix_exception), FailureCategory::engine_fault};
}

// The flat spine list in canonical BFS order: `root` plus everything reachable
// through next_blocks. Every consumer of "the spine" (prefilter inventory,
// post-process input, basic-actions fallback) wants exactly this.
//
// Not usable for the matcher walk, which must read next_blocks after try_build
// ran on the node, because a merge mutates the tree mid-walk.
std::vector<Block *> collect_spine(Block *root) {
  std::vector<Block *> out;
  std::deque<Block *> queue{root};
  while (!queue.empty()) {
    Block *cur = queue.front();
    queue.pop_front();
    out.push_back(cur);
    for (Block *n : cur->next_blocks) {
      queue.push_back(n);
    }
  }
  return out;
}

// Scrub non-owning Block values before the TraceContext arena dies.
void scrub_value(Value &v) {
  if (v.t == VType::Block) {
    v = Value::null();
    return;
  }
  if (v.items) {
    for (Value &item : *v.items) {
      scrub_value(item);
    }
  }
  if (v.fields) {
    for (auto &[key, field] : *v.fields) {
      scrub_value(field);
    }
  }
}

void scrub_arena_refs(Action &a) {
  for (Value *v : {
#define MCH_ACTION_REF(name) &a.name,
           MCH_ACTION_COLUMNS(MCH_ACTION_REF)
#undef MCH_ACTION_REF
           &a.data,
       }) {
    scrub_value(*v);
  }
}

bool try_build(const CompiledMatcher &m, const BuildFn &fn, Block *block,
               BlockArena &arena, const std::set<std::string> &kinds,
               const LookupSource &src, const WhereTable &wheres,
               std::vector<CoreAction> &out) {
  auto res = matcher_match(m, block, wheres);
  if (!res) {
    return false;
  }
  std::vector<Value> slots = slots_from_captures(res->captures);
  std::vector<Value> consumed_vals;
  for (Block *b : res->consumed) {
    consumed_vals.push_back(Value::make_block(b));
  }
  bool needs_lookups = !(m.ref_lookups.empty() && m.ref_fns.empty());
  RejectCtx rctx = reject_ctx();
  rctx.matcher = &m.name;
  RejectScope reject_scope(rctx);
  BuildOutcome o = run_two_phase(fn, block, slots,
                                 Value::make_list(std::move(consumed_vals)), kinds, src,
                                 needs_lookups);
  if (o.is_rejected) {
    return false;
  }

  Block *produced = arena.make(o.btype.empty() ? m.produces.front() : o.btype);
  produced->data = o.data;
  produced->failed = o.failed;
  produced->broken = o.broken;
  if (!produced->merge_blocks(res->consumed)) {
    return false;  // merge failure is a clean rejection
  }

  if (!m.ref_shapers.empty()) {
    const std::string &shaper_name = *m.ref_shapers.begin();
    auto sit = host_shapers().find(shaper_name);
    if (sit != host_shapers().end()) {
      ShaperMatch sm;
      sm.arena = &arena;
      sm.consumed = res->consumed;
      for (const Capture &c : res->captures) {
        sm.captures[c.name] = c.vals.empty() ? nullptr : c.vals.front();
      }
      sit->second(produced, sm);
    }
  }

  out.push_back(CoreAction{m.name, block, produced});
  return true;
}

// Promote marker blocks of one btype (contract_deploy, gasless_request) nested
// in children to the top-level spine so serialization includes them. The index
// loop must see appends because it re-scans promoted markers.
void unwind_markers(std::vector<Block *> &blocks, const char *btype) {
  std::set<Block *> visited;
  for (std::size_t i = 0; i < blocks.size(); i++) {  // index loop: sees appends
    std::deque<Block *> queue(blocks[i]->children_blocks.begin(),
                              blocks[i]->children_blocks.end());
    while (!queue.empty()) {
      Block *child = queue.front();
      queue.pop_front();
      if (child->btype == btype && visited.count(child) == 0) {
        blocks.push_back(child);
      } else {
        for (Block *c : child->children_blocks) queue.push_back(c);
      }
      visited.insert(child);
    }
  }
}

// Order is load-bearing: V1 merge before V2 pairing, markers unwound last.
void run_post_processors(std::vector<Block *> &spine, BlockArena &arena,
                         std::set<Block *> &removed) {
  post_process_dedust_liquidity(spine, removed);
  post_process_dedust_v2_liquidity(spine, removed);
  post_process_stonfi_v2_liquidity(spine, arena, removed);
  unwind_markers(spine, mch::btype::kContractDeploy);
  unwind_markers(spine, mch::btype::kGaslessRequest);
}

// A spine block becomes a row unless it is the root wrapper, an `empty`
// proxy, or a call_contract whose message has a null source (external-in)
// or a null destination (log / notification).
bool serializes_to_row(const Block *b) {
  if (b->btype == mch::btype::kRoot || b->btype == mch::btype::kEmpty) return false;
  if (b->btype != mch::btype::kCallContract) return true;
  return !pp_field(b->data, "source").is_null() && !pp_field(b->data, "destination").is_null();
}

// Btypes whose merged children stay merged. Every other btype expands
// children into their own rows so a v1 consumer still sees absorbed legs.
// A btype missing here silently starts emitting child rows.
bool is_v1_op(const std::string &t) {
  static const std::set<std::string> ops{
      mch::btype::kCallContract,
      mch::btype::kContractDeploy,
      mch::btype::kJettonBurn,
      mch::btype::kTickTock,
      mch::btype::kJettonTransfer,
      mch::btype::kNftTransfer,
      mch::btype::kNftMint,
      mch::btype::kJettonMint,
      mch::btype::kTonTransfer,
      mch::btype::kDexDepositLiquidity,
      mch::btype::kJettonSwap,
      mch::btype::kChangeDns,
      mch::btype::kDeleteDns,
      mch::btype::kRenewDns,
      mch::btype::kSubscribe,
      mch::btype::kDexWithdrawLiquidity,
      mch::btype::kUnsubscribe,
      mch::btype::kElectionDeposit,
      mch::btype::kElectionRecover,
      mch::btype::kAuctionBid,
      mch::btype::kNominatorPoolDeposit,
      mch::btype::kNominatorPoolWithdrawRequest,
      mch::btype::kDedustDepositLiquidity,
      mch::btype::kDedustDepositLiquidityPartial,
      mch::btype::kDedustV2DepositLiquidity,
      mch::btype::kDedustV2DepositLiquidityPartial,
      mch::btype::kTonstakersDeposit,
      mch::btype::kTonstakersWithdrawRequest,
      mch::btype::kTonstakersWithdraw,
      mch::btype::kEthenaWithdrawalRequest,
      mch::btype::kEthenaDeposit,
      mch::btype::kToncoDepositLiquidity,
      mch::btype::kToncoWithdrawLiquidity,
      mch::btype::kCoffeeDepositLiquidity,
      mch::btype::kChangeWalletKey,
      mch::btype::kGaslessRequest,
  };
  return ops.count(t) != 0;
}

// Links every row that is the immediate result of a relayed (gas-free) request
// to its gasless_request marker: the request message itself (when a matcher
// merged it into a composite) and its direct children. Anything deeper is the
// receiving wallet's own step. Nested relaying chains marker to marker.
void link_gasless_parents(std::vector<ActionRow> &rows) {
  std::map<const EventNode *, std::string> marker_ids;
  for (const ActionRow &row : rows) {
    if (row.parent_action_id.empty() && row.block->btype == mch::btype::kGaslessRequest) {
      marker_ids[root_event_node(row.block)] = row.action_id;
    }
  }
  if (marker_ids.empty()) return;
  for (ActionRow &row : rows) {
    const EventNode *node = root_event_node(row.block);
    const std::string &t = row.block->btype;
    auto it = marker_ids.end();
    if (t != mch::btype::kGaslessRequest && t != mch::btype::kCallContract) {
      it = marker_ids.find(node);
    }
    if (it == marker_ids.end() && node != nullptr && node->parent != nullptr) {
      it = marker_ids.find(node->parent);
    }
    if (it != marker_ids.end()) row.parent_gasless_action = it->second;
  }
}

// Filter and recursively serialize action rows.
std::vector<ActionRow> serialize_rows(const std::vector<Block *> &blocks,
                                      const std::string &parent_id) {
  std::vector<ActionRow> out;
  std::set<std::string> ids;
  for (Block *b : blocks) {
    if (!serializes_to_row(b)) continue;
    std::string id = calc_action_id(b);
    if (!parent_id.empty() && id == parent_id) continue;
    out.push_back(ActionRow{b, id, parent_id, {}});
    ids.insert(id);
    if (is_v1_op(b->btype)) continue;
    for (ActionRow &c : serialize_rows(b->children_blocks, id)) {
      // Markers were unwound onto the spine; a second copy here would collide.
      if (c.block->btype == mch::btype::kContractDeploy ||
          c.block->btype == mch::btype::kGaslessRequest) {
        continue;
      }
      c.ancestor_type.push_back(b->btype);
      std::sort(c.ancestor_type.begin(), c.ancestor_type.end());
      c.ancestor_type.erase(std::unique(c.ancestor_type.begin(), c.ancestor_type.end()),
                            c.ancestor_type.end());
      // Raise rather than drop: two rows with one action_id collide on the
      // actions PK, so the trace falls back.
      if (!ids.insert(c.action_id).second) {
        throw std::runtime_error("duplicate action id in child recursion");
      }
      out.push_back(std::move(c));
    }
  }
  if (parent_id.empty()) link_gasless_parents(out);  // top-level call only
  return out;
}

// Fresh leaf-only classification with no matchers, followed by post-processing
// and serialization. Any exception produces an empty fallback.
std::vector<ActionRow> basic_classify_fallback(TraceContext &ctx) {
  std::vector<ActionRow> out;
  try {
    Block *root = init_block(ctx.arena, ctx.tree.root);
    Block *wrap = wrap_root(ctx.arena, root);
    std::vector<Block *> spine = collect_spine(wrap);
    std::set<Block *> removed;
    run_post_processors(spine, ctx.arena, removed);
    // Recursive serialization; basic btypes are all v1_ops, so recursion
    // normally has no additional rows.
    out = serialize_rows(spine, "");
  } catch (...) {
    out.clear();  // fallback returns empty on any exception
  }
  return out;
}

// If normal serialization is empty, synthesize rows for the unsent messages of a
// lone failed wallet external. This uses a fresh block tree and falls back to the
// unknown row on any exception.
std::vector<ActionRow> ghost_external_rows(TraceContext &ctx, const LookupSource &src) {
  std::vector<ActionRow> out;
  try {
    EventNode *root_node = ctx.tree.root;
    // "Only external in allowed": a childless root whose message is an ext-in.
    if (root_node == nullptr || !root_node->children.empty() || root_node->msg == nullptr ||
        root_node->msg->source) {
      return out;
    }
    if (synthesize_ghost_children(ctx.tree, root_node) == 0) {
      return out;
    }
    Block *b = init_block(ctx.arena, root_node);
    Block *wrapper = wrap_root(ctx.arena, b);
    // Same BFS as the main loop (next_blocks read AFTER the build,
    // parent-is-None guard).
    std::deque<Block *> queue{wrapper};
    while (!queue.empty()) {
      Block *cur = queue.front();
      queue.pop_front();
      if (cur->parent == nullptr) {
        fallback_jetton_transfer(cur, ctx.arena, src);
      }
      for (Block *n : cur->next_blocks) {
        queue.push_back(n);
      }
    }
    std::vector<Block *> spine = collect_spine(wrapper);
    std::set<Block *> removed;
    run_post_processors(spine, ctx.arena, removed);
    // Flat: apply the row filter directly, so a ghost row never recurses
    // into children and never carries parent_action_id.
    for (Block *sb : spine) {
      if (serializes_to_row(sb)) out.push_back(ActionRow{sb, calc_action_id(sb), "", {}});
    }
  } catch (...) {
    out.clear();
  }
  return out;
}

}  // namespace

ClassifySetup prepare_classify(const std::vector<CompiledMatcher> &matchers) {
  ClassifySetup s;
  // Matcher / build / where tables are three views of one document. Disagreeing
  // shas mean the tables would run matchers against another document's build
  // programs — a hard setup error.
  const std::string sha = gen_matchers_ir_source_sha();
  if (sha != gen_builds_ir_source_sha() || sha != gen_wheres_ir_source_sha()) {
    s.error = "generated tables disagree on their source document (matchers " + sha + ", builds " +
              gen_builds_ir_source_sha() + ", wheres " + gen_wheres_ir_source_sha() +
              ") — reconfigure and rebuild ton-mch-engine";
    return s;
  }
  for (const GenWhere &g : gen_wheres_ir()) {
    s.where_fns.emplace(g.node_id, g.fn);
  }
  for (int i = 0; i < static_cast<int>(matchers.size()); i++) {
    std::string reason = match_skip_reason(matchers[i]);
    if (reason.empty()) {
      reason = build_skip_reason(matchers[i]);
    }
    if (!reason.empty()) {
      s.skips.emplace_back(matchers[i].name, reason);
      continue;
    }
    s.kinds.insert(matchers[i].ref_lookups.begin(), matchers[i].ref_lookups.end());
    s.included.push_back(i);
  }
  std::sort(s.skips.begin(), s.skips.end());
  for (const GenBuild &g : gen_builds_ir()) {
    s.build_fns.emplace(g.id, g.fn);
  }
  for (int idx : s.included) {
    const CompiledMatcher &m = matchers[idx];
    if (s.build_fns.find(m.artifact_index) == s.build_fns.end()) {
      s.error = "generated build table has no fn for matcher '" + m.name + "' (artifact index " +
                std::to_string(m.artifact_index) + ") — rebuild ton-mch-engine";
      return s;
    }
    // Where coverage: every where_expr node of a matcher the loop will run must
    // have a compiled fn, or eval_where would silently fail that node's head
    // test (a wrong match, not a loud error).
    for (const CompiledNode &n : m.nodes) {
      if (n.has_where_expr && s.where_fns.find(n.global_id) == s.where_fns.end()) {
        s.error = "generated where table has no fn for node " + std::to_string(n.global_id) +
                  " (matcher '" + m.name + "') — rebuild ton-mch-engine";
        return s;
      }
    }
  }
  return s;
}

ClassifyResult classify_trace(TraceContext &ctx, const std::vector<CompiledMatcher> &matchers,
                              const ClassifySetup &setup, const LookupSource &src) {
  ClassifyResult result;

  // Name the trace on every [mch-reject] line this classify emits.
  RejectCtx rctx;
  rctx.trace_id = &ctx.trace.trace_id;
  RejectScope reject_scope(rctx);

  Block *wrapper = wrap_root(ctx.arena, ctx.root);

  // Anchor prefilter: opcodes + btypes on the spine. A matcher whose
  // opcode/btype anchor intersects neither can match nothing, so its
  // traversal is skipped. Opcodes are static (produced composites carry
  // none); btypes grow as matchers fire, so produced btypes are folded in
  // after each matcher. Consumed blocks are never removed, so a skip only
  // fires when the traversal provably matches nothing.
  std::unordered_set<std::uint32_t> inv_opcodes;
  std::unordered_set<std::string> inv_btypes;
  for (Block *b : collect_spine(wrapper)) {
    inv_btypes.insert(b->btype);
    if (b->opcode) inv_opcodes.insert(*b->opcode);
  }
  // Opcode, btype, and mixed anchors are prefilterable from the block's own
  // opcode/btype. Predicate anchors stay full-scan.
  auto anchor_can_match = [&](const CompiledMatcher &m) -> bool {
    if (m.anchor_kind == AnchorKind::OpcodeSet || m.anchor_kind == AnchorKind::Mixed) {
      for (std::uint32_t op : m.anchor_opcodes) {
        if (inv_opcodes.count(op)) return true;
      }
      if (m.anchor_kind == AnchorKind::OpcodeSet) return false;
    }
    if (m.anchor_kind == AnchorKind::BType || m.anchor_kind == AnchorKind::Mixed) {
      for (const std::string &bt : m.anchor_btypes) {
        if (inv_btypes.count(bt)) return true;
      }
      return false;
    }
    return true;
  };

  std::vector<CoreAction> pending;
  try {
    for (int idx : setup.included) {
      const CompiledMatcher &m = matchers[idx];
      if (!anchor_can_match(m)) {
        continue;  // No spine op/btype can satisfy this anchor; skip traversal.
      }
      auto fn = setup.build_fns.at(m.artifact_index);
      std::size_t produced_from = pending.size();
      // FIFO; cur's next_blocks are read after try_build ran on cur. Merges
      // mutate the tree mid-walk.
      std::deque<Block *> queue{wrapper};
      while (!queue.empty()) {
        Block *cur = queue.front();
        queue.pop_front();
        if (cur->parent == nullptr) {
          try_build(m, fn, cur, ctx.arena, setup.kinds, src, setup.where_fns, pending);
        }
        for (Block *n : cur->next_blocks) {
          queue.push_back(n);
        }
      }
      // Fold this matcher's produced btypes into the inventory so a later
      // btype-anchored matcher composing on them is not wrongly skipped.
      for (std::size_t j = produced_from; j < pending.size(); j++) {
        if (pending[j].produced != nullptr) inv_btypes.insert(pending[j].produced->btype);
      }
    }

    std::vector<Block *> spine = collect_spine(wrapper);
    std::set<Block *> removed;
    run_post_processors(spine, ctx.arena, removed);
    for (CoreAction &a : pending) {
      if (!removed.count(a.produced)) result.actions.push_back(std::move(a));
    }
    // Row set is the spine, not the fire list. A fire whose product a later
    // matcher consumed is off the spine unless its consumer's btype is
    // outside v1_ops, in which case recursion brings it back as a child row;
    // a leaf no matcher consumed emits a basic row.
    result.action_rows = serialize_rows(spine, "");
  } catch (...) {
    auto [reason, category] = exception_failure(std::current_exception(), false);
    result.failure = true;
    result.failure_reason = std::move(reason);
    result.failure_category = category;
  }
  if (result.failure) {
    result.actions.clear();
    result.action_rows.clear();
    result.fallback_rows = basic_classify_fallback(ctx);
  } else if (result.action_rows.empty() && !ctx.trace.transactions.empty()) {
    // Fall back to ghost external rows before minting the unknown action.
    result.action_rows = ghost_external_rows(ctx, src);
    result.unknown_trace = result.action_rows.empty();
  }
  return result;
}

SchemaClassifyResult classify_schema_trace(
    const MchEnginePrep &prep, const std::string &trace_id,
    const std::vector<schema::Transaction> &txs, TieredLookupSource &src) {
  SchemaClassifyResult result;
  result.rescue = create_unknown_action(trace_id, txs);
  if (txs.size() == 1 &&
      std::holds_alternative<schema::TransactionDescr_tick_tock>(txs.front().description)) {
    result.outcome = TraceOutcome::skipped_lone_tick_tock;
    return result;
  }
  bool completed = false;
  try {
    TraceContext ctx;
    ctx.trace = schema_to_trace(trace_id, txs).move_as_ok();
    ctx.tree = to_tree(ctx.trace);
    if (ctx.tree.root == nullptr) {
      result.outcome = TraceOutcome::terminal_failure_empty;
      result.failure_reason = "empty event tree";
      result.failure_category = FailureCategory::malformed_trace;
      return result;
    }
    ctx.root = init_block(ctx.arena, ctx.tree.root);

    ClassifyResult classified = classify_trace(ctx, *prep.matchers, prep.setup, src);
    result.outcome = classified.failure ? TraceOutcome::fallback : TraceOutcome::classified;
    result.failure_reason = classified.failure_reason;
    result.failure_category = classified.failure_category;

    const std::vector<ActionRow> &core =
        classified.failure ? classified.fallback_rows : classified.action_rows;
    for (const ActionRow &row : core) {
      Action action;
      // Action construction and payload validation are isolated per action.
      // Other engine faults still reach the trace-level handler below.
      try {
        if (!build_action(row, action)) {
          continue;
        }
        scrub_arena_refs(action);
        action.encoded_payload = encode_action_payload(action);
      } catch (const std::runtime_error &error) {
        ++result.skipped_payload_actions;
        if (!result.failure_reason.empty()) result.failure_reason += "; ";
        result.failure_reason += error.what();
        LOG(ERROR) << "trace=" << trace_id << " action=" << action.action_id
                   << " " << error.what();
        continue;
      }
      result.rows.push_back(std::move(action));
    }

    if (classified.unknown_trace ||
        (!classified.failure && result.rows.empty() && result.skipped_payload_actions > 0)) {
      result.rows.push_back(result.rescue);
      result.unknown_row = true;
    }
    if (classified.failure && result.rows.empty()) {
      result.outcome = TraceOutcome::terminal_failure_empty;
    }
    completed = true;
  } catch (...) {
    result.outcome = TraceOutcome::terminal_failure_empty;
    auto [reason, category] = exception_failure(std::current_exception(), true);
    result.failure_reason = std::move(reason);
    result.failure_category = category;
  }
  if (!completed) {
    result.rows.clear();
    result.unknown_row = false;
  }
  return result;
}

}  // namespace mch
