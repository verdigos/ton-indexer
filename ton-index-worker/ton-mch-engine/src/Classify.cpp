#include "Classify.h"

#include "BlockTree.h"
#include "BuildDriver.h"
#include "BuildRuntime.h"
#include "GenBuilds.h"
#include "IrLoader.h"
#include "Walker.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace mch {

namespace {

constexpr std::uint32_t kExcessOpcode = 0xD53276DB;
constexpr std::uint32_t kBounceOpcode = 0xFFFFFFFF;

// engine.py IrMatcher.try_build ported: returns true iff a block was produced
// (and prints the ACTION line). Rejections and mismatches print nothing.
bool try_build(const CompiledMatcher &m, BuildOutcome (*fn)(BuildEnv &), Block *block,
               BlockArena &arena, const std::set<std::string> &kinds,
               const FixtureLookupSource &src) {
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

  std::vector<std::string> ck;
  for (Block *cb : produced->children_blocks) {
    ck.push_back(block_key(cb));
  }
  std::sort(ck.begin(), ck.end());
  std::string consumed_s = "[";
  for (std::size_t i = 0; i < ck.size(); i++) {
    if (i) consumed_s += ",";
    consumed_s += ck[i];
  }
  consumed_s += "]";

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

  std::printf("ACTION %s anchor=%s btype=%s failed=%d broken=%d data=%s flows=%s "
              "consumed=%s auto=%s\n",
              m.name.c_str(), block_key(block).c_str(), produced->btype.c_str(),
              produced->failed ? 1 : 0, produced->broken ? 1 : 0,
              render_value(produced->data).c_str(), flows.c_str(), consumed_s.c_str(),
              aux_s.c_str());
  return true;
}

}  // namespace

int run_classify(const std::string &artifact_path, const std::vector<std::string> &paths) {
  auto r_ir = load_ir(artifact_path);
  if (r_ir.is_error()) {
    std::fprintf(stderr, "failed to load IR artifact: %s\n", r_ir.error().message().c_str());
    return 2;
  }
  LoadedIr ir = r_ir.move_as_ok();

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
            try_build(m, fn, cur, ctx.arena, kinds, src);
          }
          for (Block *n : cur->next_blocks) {
            queue.push_back(n);
          }
        }
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
