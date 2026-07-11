#include "Classify.h"

#include "BlockTree.h"
#include "BuildDriver.h"
#include "BuildRuntime.h"
#include "ExprRuntime.h"
#include "GenBuilds.h"
#include "HostRegistry.h"
#include "IrLoader.h"
#include "Walker.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace mch {

namespace {

constexpr std::uint32_t kExcessOpcode = 0xD53276DB;
constexpr std::uint32_t kBounceOpcode = 0xFFFFFFFF;

// A produced action, COLLECTED during the matcher loop and emitted only after
// the post-process passes run (Phase E): the passes merge/remove produced
// blocks, so `data`/`consumed` must be rendered from the FINAL block state, not
// at fire time. aux/flows are fire-time (the anchor's excess/bounce, unchanged
// by pairing).
struct PendingAction {
  std::string matcher_name;
  std::string anchor_key;
  Block *produced;
  std::string aux_s;
  std::string flows_s;
};

// engine.py IrMatcher.try_build ported: returns true iff a block was produced
// (collects a PendingAction). Rejections and mismatches collect nothing.
bool try_build(const CompiledMatcher &m, BuildOutcome (*fn)(BuildEnv &), Block *block,
               BlockArena &arena, const std::set<std::string> &kinds,
               const FixtureLookupSource &src, std::vector<PendingAction> &out) {
  auto res = matcher_match(m, block);  // test_self + walk (root absorb=False)
  if (!res) {
    return false;  // structural mismatch
  }
  std::vector<Value> slots = slots_from_captures(res->captures);
  std::vector<Value> consumed_vals;
  for (Block *b : res->consumed) {
    consumed_vals.push_back(Value::make_block(b));
  }
  BuildOutcome o = run_two_phase(fn, block, slots,
                                 Value::make_list(std::move(consumed_vals)), kinds, src);
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
  // The shaper receives the produced block and a match view carrying the
  // capture bindings (scalar name -> block) and the consumed set.
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

  std::string aux_s = "[";
  for (std::size_t i = 0; i < aux.size(); i++) {
    if (i) aux_s += ",";
    aux_s += block_key(aux[i]);
  }
  aux_s += "]";

  std::string flows = "[";
  for (std::size_t i = 0; i < o.flows.size(); i++) {
    if (i) flows += ",";
    flows += o.flows[i].kind + "(";
    for (std::size_t j = 0; j < o.flows[i].args.size(); j++) {
      if (j) flows += ",";
      flows += render_value(o.flows[i].args[j]);
    }
    flows += ")";
  }
  flows += "]";

  out.push_back(PendingAction{m.name, block_key(block), produced, std::move(aux_s), std::move(flows)});
  return true;
}

// --- Phase E: host post-process passes (blocks/liquidity.py) -----------------
// value_flow / contract_deployments are DROPPED from C++ scope (plan §7 banner);
// the passes only reproduce the block-tree + data effects the classify dump
// renders. All surgery is throw-safe (no aborts).

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
// catches -> the pair is left unmerged, matching a Python raise's net effect of
// no merge for that pair over the corpus, which never trips these).
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

  // exactly 2 unique assets, matching the target set.
  if (tuples.size() != 2) throw std::runtime_error("combine_deposits: expected 2 unique assets");
  bool ut = (pp_eq(tuples[0].asset, tf1) && pp_eq(tuples[1].asset, tf2)) ||
            (pp_eq(tuples[0].asset, tf2) && pp_eq(tuples[1].asset, tf1));
  if (!ut) throw std::runtime_error("combine_deposits: assets don't match target");

  // result = final copy, overwrite asset/amount/wallet_{1,2} + target ordering.
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

// blocks/liquidity.py post_process_dedust_liquidity: pair each partial deposit
// with a final deposit sharing a deposit_contract, merge the data into the
// final, and drop the partial from the spine list.
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
    // dedup children_blocks (Python list(set(...)); order washed out by the
    // sorted `consumed` rendering).
    std::sort(final_b->children_blocks.begin(), final_b->children_blocks.end());
    final_b->children_blocks.erase(
        std::unique(final_b->children_blocks.begin(), final_b->children_blocks.end()),
        final_b->children_blocks.end());
    final_b->calculate_min_max_lt();
  }
}

// blocks/liquidity.py post_process_stonfi_v2_liquidity (R08 pairing pass). ZERO
// corpus coverage (no StonfiV2ProvideLiquidity messages in the fixtures) — the
// `< 2 legs` guard returns immediately for every corpus trace. Ported faithfully
// for future coverage; value_flow / contract_deployments / initiating_event_node
// are out of C++ scope (calculate_min_max_lt recomputes lt from the tree).
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
// The consumed in-leg head: the unique child whose previous_block is `proxied`.
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
        // _stonfi_v2_merge_paired: EmptyBlock proxy between the shared parent and
        // [partial, in_transfer], then absorb the partial + proxy into `final`.
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
        // _stonfi_v2_absorb_unpaired: absorb sibling + shared parent, no data.
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

// Render + print a collected action from its FINAL (post-pass) state.
void emit_action(const PendingAction &a) {
  std::vector<std::string> ck;
  for (Block *cb : a.produced->children_blocks) ck.push_back(block_key(cb));
  std::sort(ck.begin(), ck.end());
  std::string consumed_s = "[";
  for (std::size_t i = 0; i < ck.size(); i++) {
    if (i) consumed_s += ",";
    consumed_s += ck[i];
  }
  consumed_s += "]";
  std::printf("ACTION %s anchor=%s btype=%s failed=%d broken=%d data=%s flows=%s "
              "consumed=%s auto=%s\n",
              a.matcher_name.c_str(), a.anchor_key.c_str(), a.produced->btype.c_str(),
              a.produced->failed ? 1 : 0, a.produced->broken ? 1 : 0,
              render_value(a.produced->data).c_str(), a.flows_s.c_str(), consumed_s.c_str(),
              a.aux_s.c_str());
}

}  // namespace

int run_classify(const std::string &artifact_path, const std::vector<std::string> &paths) {
  auto r_ir = load_ir(artifact_path);
  if (r_ir.is_error()) {
    std::fprintf(stderr, "failed to load IR artifact: %s\n", r_ir.error().message().c_str());
    return 2;
  }
  LoadedIr ir = r_ir.move_as_ok();
  warn_missing_artifact_parsers(ir);

  const char *table_kind = nullptr;
  const std::vector<GenBuild> *table = select_gen_builds(ir.source_sha256, &table_kind);
  if (table == nullptr) {
    std::fprintf(stderr,
                 "no generated build table matches this artifact (loaded sha %s; ir table %s, "
                 "testbed table %s) — the CMake build pairs tables with ir/mch_ir.json and the "
                 "optional -DMCH_TESTBED_IR artifact; rebuild ton-mch-engine or pass a paired "
                 "artifact\n",
                 ir.source_sha256.c_str(), gen_builds_ir_source_sha(),
                 *gen_builds_testbed_source_sha() ? gen_builds_testbed_source_sha() : "(stub)");
    return 2;
  }

  std::vector<int> included;
  std::vector<std::pair<std::string, std::string>> skips;
  std::set<std::string> kinds;
  for (int i = 0; i < static_cast<int>(ir.matchers.size()); i++) {
    std::string reason = build_skip_reason(ir.matchers[i]);
    if (!reason.empty()) {
      skips.emplace_back(ir.matchers[i].name, reason);
      continue;
    }
    included.push_back(i);
    kinds.insert(ir.matchers[i].ref_lookups.begin(), ir.matchers[i].ref_lookups.end());
  }
  std::sort(skips.begin(), skips.end());
  for (const auto &s : skips) {
    std::printf("SKIP %s reason=%s\n", s.first.c_str(), s.second.c_str());
  }

  std::unordered_map<int, BuildOutcome (*)(BuildEnv &)> build_fns;
  for (const GenBuild &g : *table) {
    build_fns.emplace(g.id, g.fn);
  }
  for (int mi : included) {
    if (build_fns.find(ir.matchers[mi].artifact_index) == build_fns.end()) {
      std::fprintf(stderr,
                   "generated %s build table has no fn for matcher '%s' (artifact index %d) — "
                   "rebuild ton-mch-engine (CMake regenerates the paired tables)\n",
                   table_kind, ir.matchers[mi].name.c_str(), ir.matchers[mi].artifact_index);
      return 2;
    }
  }

  namespace fs = std::filesystem;
  std::vector<std::string> fixtures;
  for (const std::string &p : paths) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
      std::vector<std::string> names;
      for (const auto &e : fs::directory_iterator(p)) {
        if (e.path().extension() == ".lz4") {
          names.push_back(e.path().string());
        }
      }
      std::sort(names.begin(), names.end());
      fixtures.insert(fixtures.end(), names.begin(), names.end());
    } else {
      fixtures.push_back(p);
    }
  }

  for (const std::string &f : fixtures) {
    std::printf("=== %s\n", fs::path(f).filename().string().c_str());
    auto r_ctx = load_trace_context(f);
    if (r_ctx.is_error()) {
      std::printf("ERROR: %s\n", r_ctx.error().message().c_str());
      continue;
    }
    auto ctx = r_ctx.move_as_ok();
    // Python to_tree asserts a single root; the C++ substrate tolerates orphan
    // subtrees but the loss must not be silent (BlockTree.cpp). Surface to
    // stderr only — stdout (the A/B channel) stays byte-identical, and no
    // corpus trace has orphans, so this never fires in the gates.
    if (ctx.tree.unlinked != 0) {
      std::fprintf(stderr, "WARN %s: %zu orphan nodes unreachable from root\n",
                   fs::path(f).filename().string().c_str(), ctx.tree.unlinked);
    }
    FixtureLookupSource src(&ctx.trace.interfaces);

    // Synthetic root wrapper (process_event_async: Block('root').connect(...)).
    Block *wrapper = ctx.arena.make("root");
    wrapper->connect(ctx.root);

    // Per-trace exception boundary (Python parity): a pred/builder/parser
    // exception escaping match/build — a recursion-depth trip (Walker
    // MatchDepthExceeded), a two-phase non-convergence (run_two_phase), a
    // pytoniq BOC/index fault, a VmError on a pruned ref — propagates OUT of
    // the engine to event_processing.py's per-trace `except Exception`, which
    // discards the trace's actions and falls back. Here that is a single
    // `TRACE_FAIL` line on stdout (the reason goes to stderr so the A/B
    // channel is byte-comparable). No corpus trace triggers this.
    std::vector<PendingAction> pending;
    try {
      for (int mi : included) {
        const CompiledMatcher &m = ir.matchers[mi];
        auto fn = build_fns.at(m.artifact_index);
        // Python bfs_iter port: FIFO; cur's next_blocks are read AFTER
        // try_build ran on cur (generator resume order) — merges mutate the
        // tree mid-walk.
        std::deque<Block *> queue{wrapper};
        while (!queue.empty()) {
          Block *cur = queue.front();
          queue.pop_front();
          if (cur->parent == nullptr) {
            try_build(m, fn, cur, ctx.arena, kinds, src, pending);
          }
          for (Block *n : cur->next_blocks) {
            queue.push_back(n);
          }
        }
      }

      // Phase E: post-process passes run AFTER the matcher loop, over the flat
      // spine list ([root] + bfs), then the SURVIVING produced actions are
      // emitted from their final state (event_processing.py
      // process_event_async_with_postprocessing).
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
      // The passes SUPPRESS only the produced blocks they merge away (a paired
      // partial); every other collected action is still emitted (the dump is
      // per-fire — a block later consumed by another matcher keeps its line).
      std::set<Block *> removed;
      post_process_dedust_liquidity(spine, removed);
      post_process_stonfi_v2_liquidity(spine, ctx.arena, removed);
      for (const PendingAction &a : pending) {
        if (!removed.count(a.produced)) emit_action(a);
      }
    } catch (const std::exception &e) {
      std::printf("TRACE_FAIL\n");
      std::fprintf(stderr, "TRACE_FAIL %s: %s\n",
                   fs::path(f).filename().string().c_str(), e.what());
    } catch (...) {
      std::printf("TRACE_FAIL\n");
      std::fprintf(stderr, "TRACE_FAIL %s: unknown exception\n",
                   fs::path(f).filename().string().c_str());
    }
  }
  return 0;
}

}  // namespace mch
