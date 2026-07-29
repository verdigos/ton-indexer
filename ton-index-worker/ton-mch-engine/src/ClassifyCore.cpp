#include "ClassifyCore.h"

#include "BuildDriver.h"
#include "ExprRuntime.h"
#include "GenBuilds.h"
#include "HostRegistry.h"
#include "Walker.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <map>

namespace mch {

namespace {

constexpr std::uint32_t kExcessOpcode = 0xD53276DB;
constexpr std::uint32_t kBounceOpcode = 0xFFFFFFFF;

// engine.py IrMatcher.try_build ported: returns true iff a block was produced
// (collects a CoreAction). Rejections and mismatches collect nothing. Renders
// NOTHING — aux/flows are stored raw for the adapter (§11 item 2 isolation).
bool try_build(const CompiledMatcher &m, BuildOutcome (*fn)(BuildEnv &), Block *block,
               BlockArena &arena, const std::set<std::string> &kinds,
               const LookupSource &src, std::vector<CoreAction> &out) {
  auto res = matcher_match(m, block);  // test_self + walk (root absorb=False)
  if (!res) {
    return false;  // structural mismatch
  }
  std::vector<Value> slots = slots_from_captures(res->captures);
  std::vector<Value> consumed_vals;
  for (Block *b : res->consumed) {
    consumed_vals.push_back(Value::make_block(b));
  }
  bool needs_lookups = !(m.ref_lookups.empty() && m.ref_fns.empty());
  BuildOutcome o = run_two_phase(fn, block, slots,
                                 Value::make_list(std::move(consumed_vals)), kinds, src,
                                 needs_lookups);
  if (o.is_rejected) {
    return false;  // clean rejection §10.7: no consumption, no aux append
  }

  Block *produced = arena.make(o.btype.empty() ? m.produces.front() : o.btype);
  produced->data = o.data;
  produced->failed = o.failed;
  produced->broken = o.broken;
  if (!produced->merge_blocks(res->consumed)) {
    return false;  // Python: merge raises -> try_build catches -> clean rejection
  }

  // §8.7 `shape` hook: LAST build step, after merge (engine.py _apply_shape).
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

  // Root-level excess/bounce auto-append (engine.py try_build): from the
  // ANCHOR's next_blocks (unchanged by the merge), canonical order.
  std::vector<Block *> aux;
  {
    std::vector<Block *> nb = block->next_blocks;
    std::stable_sort(nb.begin(), nb.end(),
                     [](const Block *a, const Block *b) { return a->min_lt < b->min_lt; });
    for (Block *n : nb) {
      if (n->btype != "call_contract" || !n->opcode) {
        continue;
      }
      if ((*n->opcode == kExcessOpcode && m.include_excess) ||
          (*n->opcode == kBounceOpcode && m.include_bounces)) {
        aux.push_back(n);
      }
    }
  }

  out.push_back(CoreAction{m.name, block, produced, std::move(aux)});
  return true;
}

// --- Phase E: host post-process passes (blocks/liquidity.py) -----------------
// value_flow / contract_deployments are DROPPED from C++ scope (plan §7 banner).
// All surgery is throw-safe (no aborts).

Value pp_field(const Value &d, const std::string &k) {
  const Value *v = d.field(k);
  return v != nullptr ? *v : Value::null();
}
bool pp_eq(const Value &a, const Value &b) {
  EvalResult r = rt_eq(a, b);
  return !r.faulted && r.value.t == VType::Bool && r.value.boolean;
}

// blocks/liquidity.py combine_deposits: merge the partial (first) + final
// deposit records. Throws on the consistency checks Python raises on (caller
// catches -> the pair is left unmerged, matching a Python raise's net effect).
Value combine_deposits(const Value &first, const Value &final_d) {
  Value tf1 = pp_field(first, "target_asset_1"), tf2 = pp_field(first, "target_asset_2");
  Value tg1 = pp_field(final_d, "target_asset_1"), tg2 = pp_field(final_d, "target_asset_2");
  bool same = (pp_eq(tf1, tg1) && pp_eq(tf2, tg2)) || (pp_eq(tf1, tg2) && pp_eq(tf2, tg1));
  if (!same) throw std::runtime_error("combine_deposits: target assets mismatch");

  struct Tup { Value asset, amount, wallet; };
  std::vector<Tup> tuples;
  auto scan = [&](const Value &d, bool merge) {
    for (int i = 1; i <= 2; i++) {
      std::string si = std::to_string(i);
      Value asset = pp_field(d, "asset_" + si);
      if (asset.is_null()) continue;
      Value amount = pp_field(d, "amount_" + si);
      Value wallet = pp_field(d, "user_jetton_wallet_" + si);
      if (merge) {
        int idx = -1;
        for (std::size_t j = 0; j < tuples.size(); j++) {
          if (pp_eq(tuples[j].asset, asset)) { idx = static_cast<int>(j); break; }
        }
        if (idx >= 0) {
          if (!pp_eq(tuples[idx].wallet, wallet)) {
            throw std::runtime_error("combine_deposits: user wallet mismatch");
          }
          EvalResult s = rt_add(tuples[idx].amount, amount);
          if (s.faulted) throw std::runtime_error("combine_deposits: amount add faulted");
          tuples[idx].amount = s.value;
          if (!wallet.is_null()) tuples[idx].wallet = wallet;
          continue;
        }
      }
      tuples.push_back({asset, amount, wallet});
    }
  };
  scan(first, false);
  scan(final_d, true);

  if (tuples.size() != 2) throw std::runtime_error("combine_deposits: expected 2 unique assets");
  bool ut = (pp_eq(tuples[0].asset, tf1) && pp_eq(tuples[1].asset, tf2)) ||
            (pp_eq(tuples[0].asset, tf2) && pp_eq(tuples[1].asset, tf1));
  if (!ut) throw std::runtime_error("combine_deposits: assets don't match target");

  Value::Fields out;
  if (final_d.fields) out = *final_d.fields;
  auto set_field = [&](const std::string &k, Value v) {
    for (auto &kv : out) {
      if (kv.first == k) { kv.second = std::move(v); return; }
    }
    out.emplace_back(k, std::move(v));
  };
  for (int i = 0; i < 2; i++) {
    std::string si = std::to_string(i + 1);
    set_field("asset_" + si, tuples[i].asset);
    set_field("amount_" + si, tuples[i].amount);
    if (!tuples[i].wallet.is_null()) set_field("user_jetton_wallet_" + si, tuples[i].wallet);
  }
  set_field("target_asset_1", tuples[0].asset);
  set_field("target_asset_2", tuples[1].asset);
  return Value::make_dict(std::move(out));
}

void post_process_dedust_liquidity(std::vector<Block *> &blocks, std::set<Block *> &removed) {
  std::vector<Block *> firsts, finals;
  std::map<std::string, int> used;
  auto contract_key = [](const Block *b) {
    Value c = pp_field(b->data, "deposit_contract");
    return c.t == VType::Account ? (c.addr_none ? std::string("addr_none") : c.str) : c.describe();
  };
  for (Block *b : blocks) {
    if (b->btype == "dedust_deposit_liquidity_partial") {
      firsts.push_back(b);
      used[contract_key(b)]++;
    } else if (b->btype == "dedust_deposit_liquidity") {
      finals.push_back(b);
      used[contract_key(b)]++;
    }
  }
  for (const auto &kv : used) {
    if (kv.second > 2) return;  // logger.warning + skip merging
  }
  for (Block *first : firsts) {
    Block *final_b = nullptr;
    for (Block *b : finals) {
      if (contract_key(b) == contract_key(first)) { final_b = b; break; }
    }
    if (final_b == nullptr) continue;
    Value merged;
    try {
      merged = combine_deposits(first->data, final_b->data);
    } catch (const std::exception &) {
      continue;  // Python raise -> pair unmerged (unreached over corpus)
    }
    blocks.erase(std::remove(blocks.begin(), blocks.end(), first), blocks.end());
    removed.insert(first);
    for (const EventNode *n : first->event_nodes) final_b->event_nodes.push_back(n);
    final_b->data = std::move(merged);
    for (Block *cb : first->children_blocks) final_b->children_blocks.push_back(cb);
    // Python does `list(set(children_blocks))` here, i.e. an order nothing
    // downstream can rely on — but the C++ substrate has to pick SOME order,
    // and sorting raw Block* sorted by ARENA ADDRESS: run-to-run stable within
    // one process, arbitrary across processes and across arena layouts. The
    // order reaches action output (unwind_deployments, the fallback rows), so
    // it must be a property of the trace, not of the allocator. Key: earliest
    // event lt, then btype, then the anchor tx hash; the pointer stays as the
    // final tiebreak so the ordering is total and the std::unique below still
    // collapses duplicates of the SAME block (identical key -> adjacent).
    std::sort(final_b->children_blocks.begin(), final_b->children_blocks.end(),
              [](const Block *a, const Block *b) {
                if (a->min_lt != b->min_lt) return a->min_lt < b->min_lt;
                if (a->btype != b->btype) return a->btype < b->btype;
                std::string ha = a->anchor_tx_hash(), hb = b->anchor_tx_hash();
                if (ha != hb) return ha < hb;
                return a < b;
              });
    final_b->children_blocks.erase(
        std::unique(final_b->children_blocks.begin(), final_b->children_blocks.end()),
        final_b->children_blocks.end());
    final_b->calculate_min_max_lt();
  }
}

// blocks/liquidity.py post_process_stonfi_v2_liquidity (R08 pairing pass). ZERO
// corpus coverage — the `< 2 legs` guard returns immediately for every trace.
bool is_stonfi_v2_leg(const Block *b) {
  if (b->btype != "dex_deposit_liquidity") return false;
  Value dex = pp_field(b->data, "dex");
  return dex.t == VType::Str && dex.str == "stonfi_v2";
}
std::pair<int, std::int64_t> stonfi_v2_fire_order(const Block *b) {
  const Block *anchor = nullptr;
  for (Block *c : b->children_blocks) {
    if (c->opcode && *c->opcode == 0x37c096dfu) { anchor = c; break; }
  }
  int depth = 0;
  for (const Block *cur = anchor; cur != nullptr && cur->previous_block != nullptr;
       cur = cur->previous_block) {
    depth++;
  }
  return {depth, b->min_lt};
}
Block *stonfi_v2_leg_head(Block *block, Block *proxied) {
  for (Block *c : block->children_blocks) {
    if (c->previous_block == proxied) return c;
  }
  return nullptr;
}
void stonfi_v2_data_merge(Block *final_b, Block *partial) {
  auto set = [&](const std::string &k, Value v) {
    if (!final_b->data.fields) return;
    for (auto &kv : *final_b->data.fields) {
      if (kv.first == k) { kv.second = std::move(v); return; }
    }
    final_b->data.fields->emplace_back(k, std::move(v));
  };
  set("amount_2", pp_field(partial->data, "amount_1"));
  set("asset_2", pp_field(partial->data, "asset_1"));
  set("sender_wallet_2", pp_field(partial->data, "sender_wallet_1"));
  if (pp_field(final_b->data, "lp_tokens_minted").is_null()) {
    set("lp_tokens_minted", pp_field(partial->data, "lp_tokens_minted"));
  }
}
void post_process_stonfi_v2_liquidity(std::vector<Block *> &blocks, BlockArena &arena,
                                      std::set<Block *> &removed) {
  std::vector<Block *> deposits;
  for (Block *b : blocks) {
    if (is_stonfi_v2_leg(b)) deposits.push_back(b);
  }
  if (deposits.size() < 2) return;  // corpus always returns here
  std::stable_sort(deposits.begin(), deposits.end(), [](const Block *a, const Block *b) {
    return stonfi_v2_fire_order(a) < stonfi_v2_fire_order(b);
  });
  std::vector<Block *> alive;
  for (Block *b : deposits) {
    Block *a = nullptr;
    for (Block *cand : alive) {
      if (cand->previous_block != nullptr && cand->previous_block == b->previous_block) a = cand;
    }
    if (a != nullptr) {
      Value as = pp_field(a->data, "sender"), bs = pp_field(b->data, "sender");
      Value ap = pp_field(a->data, "pool"), bp = pp_field(b->data, "pool");
      Value al = pp_field(a->data, "lp_tokens_minted"), bl = pp_field(b->data, "lp_tokens_minted");
      Block *proxied = a->previous_block;
      if (pp_eq(as, bs) && pp_eq(ap, bp) && !pp_eq(al, bl)) {
        Block *in_transfer = stonfi_v2_leg_head(b, proxied);
        Block *proxy = arena.make("empty");
        if (proxied != nullptr && in_transfer != nullptr) {
          proxied->insert_between({a, in_transfer}, proxy);
        }
        b->children_blocks.push_back(a);
        b->children_blocks.push_back(proxy);
        a->parent = b;
        proxy->parent = b;
        for (const EventNode *n : a->event_nodes) b->event_nodes.push_back(n);
        b->calculate_min_max_lt();
        stonfi_v2_data_merge(b, a);
      } else {
        b->children_blocks.push_back(a);
        if (proxied != nullptr) b->children_blocks.push_back(proxied);
        a->parent = b;
        if (proxied != nullptr) proxied->parent = b;
        for (const EventNode *n : a->event_nodes) b->event_nodes.push_back(n);
        b->calculate_min_max_lt();
      }
      blocks.erase(std::remove(blocks.begin(), blocks.end(), a), blocks.end());
      alive.erase(std::remove(alive.begin(), alive.end(), a), alive.end());
      removed.insert(a);
    }
    alive.push_back(b);
  }
}

// event_processing.py unwind_deployments (3rd production post-processor, Step
// 2b): promote ContractDeploy blocks nested in children up to the top-level
// spine list. The Python `for block in blocks` loop SEES appends (it re-scans
// the promoted deploys), so the C++ index loop must too. NOTE: a no-op for the
// classify/actions DUMP — the dump emits matcher-produced blocks (`pending`),
// and this only appends to the spine, which the dump never walks; ported to
// keep the post-processor chain aligned with production for Phase G.
void unwind_deployments(std::vector<Block *> &blocks) {
  std::set<Block *> visited;
  for (std::size_t i = 0; i < blocks.size(); i++) {  // index loop: sees appends
    std::deque<Block *> queue(blocks[i]->children_blocks.begin(),
                              blocks[i]->children_blocks.end());
    while (!queue.empty()) {
      Block *child = queue.front();
      queue.pop_front();
      if (child->btype == "contract_deploy" && visited.count(child) == 0) {
        blocks.push_back(child);
      } else {
        for (Block *c : child->children_blocks) queue.push_back(c);
      }
      visited.insert(child);
    }
  }
}

// try_classify_basic_actions (event_processing.py, Step 2c): a FRESH leaf-only
// classify (init_block over the same event tree, NO matchers) + the
// post-processor chain; one CoreAction per surviving spine block of a basic
// btype. serialize_blocks skip conditions applied: skip root/empty, and
// call_contract with a null source or destination. Throw-safe (Python's
// fallback returns [] on any exception). Ungated (corpus has no failing traces).
std::vector<CoreAction> basic_classify_fallback(TraceContext &ctx) {
  std::vector<CoreAction> out;
  try {
    Block *root = init_block(ctx.arena, ctx.tree.root);
    Block *wrap = ctx.arena.make("root");
    wrap->connect(root);
    std::vector<Block *> spine;
    {
      std::deque<Block *> q{wrap};
      while (!q.empty()) {
        Block *cur = q.front();
        q.pop_front();
        spine.push_back(cur);
        for (Block *n : cur->next_blocks) q.push_back(n);
      }
    }
    std::set<Block *> removed;
    post_process_dedust_liquidity(spine, removed);
    post_process_stonfi_v2_liquidity(spine, ctx.arena, removed);
    unwind_deployments(spine);
    for (Block *b : spine) {
      if (b->btype == "root" || b->btype == "empty" || removed.count(b) != 0) continue;
      if (b->btype == "call_contract") {
        if (pp_field(b->data, "source").is_null() ||
            pp_field(b->data, "destination").is_null()) {
          continue;
        }
      }
      out.push_back(CoreAction{"basic", b, b, {}});
    }
  } catch (...) {
    out.clear();  // Python: fallback returns [] on any exception
  }
  return out;
}

}  // namespace

ClassifySetup prepare_classify(const LoadedIr &ir) {
  ClassifySetup s;
  const char *table_kind = nullptr;
  const std::vector<GenBuild> *table = select_gen_builds(ir.source_sha256, &table_kind);
  if (table == nullptr) {
    s.table_missing = true;
    s.error = "no generated build table matches this artifact (loaded sha " + ir.source_sha256 +
              "; ir table " + gen_builds_ir_source_sha() + ", testbed table " +
              (*gen_builds_testbed_source_sha() ? gen_builds_testbed_source_sha() : "(stub)") +
              ") — rebuild ton-mch-engine or pass a paired artifact";
    return s;
  }
  for (int i = 0; i < static_cast<int>(ir.matchers.size()); i++) {
    std::string reason = build_skip_reason(ir.matchers[i]);
    if (!reason.empty()) {
      s.skips.emplace_back(ir.matchers[i].name, reason);
      continue;
    }
    s.kinds.insert(ir.matchers[i].ref_lookups.begin(), ir.matchers[i].ref_lookups.end());
    // B1: precompute the anchor prefilter. Only opcode/btype anchors are
    // prefilterable; pred (and never-runnable mixed) anchors stay full-scan.
    AnchorFilter af;
    const CompiledMatcher &m = ir.matchers[i];
    if (m.anchor_kind == AnchorKind::OpcodeSet) {
      af.prefilter = true;
      af.opcodes = &m.anchor_opcodes;
    } else if (m.anchor_kind == AnchorKind::BType) {
      af.prefilter = true;
      af.btypes = &m.anchor_btypes;
    }
    s.included.push_back(IncludedMatcher{i, af});
  }
  std::sort(s.skips.begin(), s.skips.end());
  for (const GenBuild &g : *table) {
    s.build_fns.emplace(g.id, g.fn);
  }
  for (const IncludedMatcher &im : s.included) {
    const CompiledMatcher &m = ir.matchers[im.matcher_index];
    if (s.build_fns.find(m.artifact_index) == s.build_fns.end()) {
      s.fn_missing = true;
      s.error = std::string("generated ") + table_kind + " build table has no fn for matcher '" +
                m.name + "' (artifact index " + std::to_string(m.artifact_index) +
                ") — rebuild ton-mch-engine";
      return s;
    }
  }
  return s;
}

ClassifyResult classify_trace(TraceContext &ctx, const LoadedIr &ir, const ClassifySetup &setup,
                              const LookupSource &src) {
  ClassifyResult result;

  // Synthetic root wrapper (process_event_async: Block('root').connect(...)).
  Block *wrapper = ctx.arena.make("root");
  wrapper->connect(ctx.root);

  // B1 anchor prefilter inventory: the opcodes + btypes present on the spine
  // (blocks reachable via next_blocks — the outer BFS's anchor candidates). A
  // matcher whose opcode/btype anchor intersects neither can match nothing, so
  // its whole traversal is skipped. Opcodes are STATIC (produced composite blocks
  // carry no opcode); btypes GROW as matchers fire (a later matcher may anchor on
  // a produced btype), so produced btypes are folded in after each matcher. The
  // inventory is a conservative superset — consumed blocks are never removed, so
  // a skip only ever fires when the traversal provably matches nothing.
  std::unordered_set<std::uint32_t> inv_opcodes;
  std::unordered_set<std::string> inv_btypes;
  {
    std::deque<Block *> q{wrapper};
    while (!q.empty()) {
      Block *cur = q.front();
      q.pop_front();
      inv_btypes.insert(cur->btype);
      if (cur->opcode) inv_opcodes.insert(*cur->opcode);
      for (Block *n : cur->next_blocks) q.push_back(n);
    }
  }
  auto anchor_can_match = [&](const AnchorFilter &af) -> bool {
    if (!af.prefilter) return true;  // pred/mixed: always full-scan
    if (af.opcodes != nullptr) {
      for (std::uint32_t op : *af.opcodes) {
        if (inv_opcodes.count(op)) return true;
      }
      return false;
    }
    if (af.btypes != nullptr) {
      for (const std::string &bt : *af.btypes) {
        if (inv_btypes.count(bt)) return true;
      }
      return false;
    }
    return true;
  };

  std::vector<CoreAction> pending;
  try {
    for (const IncludedMatcher &im : setup.included) {
      if (!anchor_can_match(im.filter)) {
        continue;  // B1: no spine op/btype can satisfy this anchor — skip traversal
      }
      const CompiledMatcher &m = ir.matchers[im.matcher_index];
      auto fn = setup.build_fns.at(m.artifact_index);
      std::size_t produced_from = pending.size();
      // Python bfs_iter port: FIFO; cur's next_blocks are read AFTER try_build
      // ran on cur (generator resume order) — merges mutate the tree mid-walk.
      std::deque<Block *> queue{wrapper};
      while (!queue.empty()) {
        Block *cur = queue.front();
        queue.pop_front();
        if (cur->parent == nullptr) {
          try_build(m, fn, cur, ctx.arena, setup.kinds, src, pending);
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

    // Phase E: post-process passes over the flat spine list ([root] + bfs).
    std::vector<Block *> spine;
    {
      std::deque<Block *> q{wrapper};
      while (!q.empty()) {
        Block *cur = q.front();
        q.pop_front();
        spine.push_back(cur);
        for (Block *n : cur->next_blocks) q.push_back(n);
      }
    }
    std::set<Block *> removed;
    post_process_dedust_liquidity(spine, removed);
    post_process_stonfi_v2_liquidity(spine, ctx.arena, removed);
    unwind_deployments(spine);  // Step 2b (no-op for the dump; production chain parity)
    for (CoreAction &a : pending) {
      if (!removed.count(a.produced)) result.actions.push_back(std::move(a));
    }
  } catch (const std::exception &e) {
    result.failure = true;
    result.failure_reason = e.what();
    // The two-phase driver throws this exact message on non-convergence — an
    // infra fault, distinct from a spec/parse engine fault. Everything else is
    // a generic engine_fault (Python's per-trace fallback boundary).
    result.failure_category =
        result.failure_reason.find("two-phase lookup did not converge") != std::string::npos
            ? FailureCategory::lookup_infra_fail
            : FailureCategory::engine_fault;
  } catch (...) {
    result.failure = true;
    result.failure_reason = "unknown exception";
    result.failure_category = FailureCategory::engine_fault;
  }
  if (result.failure) {
    result.actions.clear();  // event_processing.py discards the trace's actions
    result.fallback_actions = basic_classify_fallback(ctx);
  } else if (result.actions.empty() && !ctx.trace.transactions.empty()) {
    // trace_processor.py:44-46 — classification succeeded and matched nothing.
    result.unknown_trace = true;
  }
  return result;
}

}  // namespace mch
