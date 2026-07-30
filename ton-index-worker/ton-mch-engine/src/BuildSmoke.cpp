#include "BuildSmoke.h"

#include "BlockTree.h"
#include "BuildDriver.h"
#include "BuildRuntime.h"
#include "fixtures/FixtureLoader.h"
#include "fixtures/FixtureLookupSource.h"
#include "fixtures/Render.h"
#include "ClassifyCore.h"
#include "IrLoader.h"
#include "Walker.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <string>
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

  // Build-table selection + runnable-matcher set + SKIP table + build-fn
  // coverage: the SAME prepare_classify() the classify path runs (this used to
  // be a hand-rolled second copy of it). The table/fn-missing diagnostics are
  // worded slightly differently than the old local strings, but they go to
  // STDERR — the dump channel (stdout SKIP lines, identical + sorted) is not
  // affected.
  ClassifySetup setup = prepare_classify(ir);
  if (setup.table_missing || setup.fn_missing) {
    std::fprintf(stderr, "%s\n", setup.error.c_str());
    return 2;
  }
  for (const auto &s : setup.skips) {
    std::printf("SKIP %s reason=%s\n", s.first.c_str(), s.second.c_str());
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
      for (int idx : setup.included) {
        const CompiledMatcher &m = ir.matchers[idx];
        auto res = matcher_match(m, block, setup.where_fns);
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
        BuildOutcome o = run_two_phase(setup.build_fns.at(m.artifact_index), block, slots, consumed,
                                       setup.kinds, src, needs_lookups);
        if (o.is_rejected) {
          std::printf("BUILD %s anchor=%s rejected\n", m.name.c_str(), block_key(block).c_str());
          continue;
        }
        std::printf("BUILD %s anchor=%s btype=%s failed=%d broken=%d data=%s\n",
                    m.name.c_str(), block_key(block).c_str(),
                    o.btype.empty() ? "-" : o.btype.c_str(), o.failed ? 1 : 0, o.broken ? 1 : 0,
                    render_value(o.data).c_str());
      }
    }
  }
  return 0;
}

}  // namespace mch
