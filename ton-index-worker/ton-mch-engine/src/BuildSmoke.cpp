#include "BuildSmoke.h"

#include "BlockTree.h"
#include "BuildDriver.h"
#include "BuildRuntime.h"
#include "GenBuilds.h"
#include "IrLoader.h"
#include "Walker.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mch {

namespace {

std::vector<Block *> gather_blocks(Block *root) {
  std::vector<Block *> out;
  std::unordered_set<Block *> seen;
  std::deque<Block *> queue{root};
  while (!queue.empty()) {
    Block *cur = queue.front();
    queue.pop_front();
    if (!seen.insert(cur).second) {
      continue;
    }
    out.push_back(cur);
    std::vector<Block *> nb = cur->next_blocks;
    std::stable_sort(nb.begin(), nb.end(),
                     [](const Block *a, const Block *b) { return a->min_lt < b->min_lt; });
    for (Block *n : nb) {
      queue.push_back(n);
    }
  }
  return out;
}

}  // namespace

int run_build_smoke(const std::string &artifact_path, const std::vector<std::string> &paths) {
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

  // Inclusion + SKIP lines (once, sorted by matcher name).
  std::vector<int> included;  // indices into ir.matchers (priority order)
  std::vector<std::pair<std::string, std::string>> skips;
  std::set<std::string> kinds;  // artifact lookup kinds served in this run
  for (int i = 0; i < static_cast<int>(ir.matchers.size()); i++) {
    const CompiledMatcher &m = ir.matchers[i];
    std::string reason = build_skip_reason(m);
    if (!reason.empty()) {
      skips.emplace_back(m.name, reason);
      continue;
    }
    included.push_back(i);
    kinds.insert(m.ref_lookups.begin(), m.ref_lookups.end());
  }
  std::sort(skips.begin(), skips.end());
  for (const auto &s : skips) {
    std::printf("SKIP %s reason=%s\n", s.first.c_str(), s.second.c_str());
  }

  // Table coverage: every included matcher needs its generated fn.
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
    FixtureLookupSource src(&ctx.trace.interfaces);

    for (Block *block : gather_blocks(ctx.root)) {
      for (int mi : included) {
        const CompiledMatcher &m = ir.matchers[mi];
        auto res = matcher_match(m, block);
        if (!res) {
          continue;
        }
        std::vector<Value> slots = slots_from_captures(res->captures);
        std::vector<Value> consumed_vals;
        for (Block *b : res->consumed) {
          consumed_vals.push_back(Value::make_block(b));
        }
        Value consumed = Value::make_list(std::move(consumed_vals));

        bool needs_lookups = !(m.ref_lookups.empty() && m.ref_fns.empty());
        BuildOutcome o = run_two_phase(build_fns.at(m.artifact_index), block, slots, consumed,
                                       kinds, src, needs_lookups);
        if (o.is_rejected) {
          std::printf("BUILD %s anchor=%s rejected\n", m.name.c_str(), block_key(block).c_str());
          continue;
        }
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
        std::printf("BUILD %s anchor=%s btype=%s failed=%d broken=%d data=%s flows=%s\n",
                    m.name.c_str(), block_key(block).c_str(),
                    o.btype.empty() ? "-" : o.btype.c_str(), o.failed ? 1 : 0, o.broken ? 1 : 0,
                    render_value(o.data).c_str(), flows.c_str());
      }
    }
  }
  return 0;
}

}  // namespace mch
