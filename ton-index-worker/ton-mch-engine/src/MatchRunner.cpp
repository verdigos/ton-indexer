#include "MatchRunner.h"

#include "BlockTree.h"
#include "BuildDriver.h"    // match_skip_reason
#include "ClassifyCore.h"    // prepare_classify
#include "GenMatchers.h"
#include "fixtures/FixtureLoader.h"
#include "fixtures/Render.h"  // shared block_key helper
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

// block_key is the shared Block dump-key helper from fixtures/Render.h.

std::string cap_val(const Capture &c) {
  if (!c.is_list) {
    return c.vals.empty() || c.vals[0] == nullptr ? "null" : block_key(c.vals[0]);
  }
  std::string out = "[";
  for (std::size_t i = 0; i < c.vals.size(); i++) {
    if (i) out += ",";
    out += c.vals[i] == nullptr ? "null" : block_key(c.vals[i]);
  }
  return out + "]";
}

// BFS from root in canonical order, dedup by identity.
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

void dump_fixture(const std::vector<const CompiledMatcher *> &runnable, const std::string &path,
                  const WhereTable &gen) {
  auto r_ctx = load_trace_context(path);
  if (r_ctx.is_error()) {
    std::printf("ERROR: %s\n", r_ctx.error().message().c_str());
    return;
  }
  auto ctx = r_ctx.move_as_ok();
  for (Block *block : gather_blocks(ctx.root)) {
    for (const CompiledMatcher *mp : runnable) {
      const CompiledMatcher &m = *mp;
      auto res = matcher_match(m, block, gen);
      if (!res) {
        continue;
      }
      std::vector<std::string> ck;
      for (Block *cb : res->consumed) {
        ck.push_back(block_key(cb));
      }
      std::sort(ck.begin(), ck.end());
      std::string consumed = "[";
      for (std::size_t i = 0; i < ck.size(); i++) {
        if (i) consumed += ",";
        consumed += ck[i];
      }
      consumed += "]";

      std::string caps = "{";
      for (std::size_t i = 0; i < res->captures.size(); i++) {
        if (i) caps += ",";
        caps += res->captures[i].name + "=" + cap_val(res->captures[i]);
      }
      caps += "}";

      std::printf("MATCH %s anchor=%s consumed=%s captures=%s\n",
                  m.name.c_str(), block_key(block).c_str(), consumed.c_str(), caps.c_str());
    }
  }
}

}  // namespace

int run_match(const std::vector<std::string> &paths) {
  // The compiled matcher table + the generated where table, through the SAME
  // prepare_classify production runs, which is also where the where-coverage
  // check lives.
  const std::vector<CompiledMatcher> &matchers = gen_matchers_ir();
  ClassifySetup setup = prepare_classify(matchers);
  if (setup.table_missing || setup.fn_missing) {
    std::fprintf(stderr, "%s\n", setup.error.c_str());
    return 2;
  }

  // The match phase runs every matcher whose PREDICATES are available, including
  // those a missing build binding would skip in classify, this dump is about the
  // walk, not about producing rows.
  std::vector<const CompiledMatcher *> runnable;
  std::vector<std::pair<std::string, std::string>> skips;
  for (const CompiledMatcher &m : matchers) {
    std::string reason = match_skip_reason(m);
    if (reason.empty()) {
      runnable.push_back(&m);
    } else {
      skips.emplace_back(m.name, std::move(reason));
    }
  }

  // SKIP lines (once, sorted by matcher name).
  std::sort(skips.begin(), skips.end());
  for (const auto &s : skips) {
    std::printf("SKIP %s reason=%s\n", s.first.c_str(), s.second.c_str());
  }

  // Expand paths: directories -> sorted *.lz4.
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
    dump_fixture(runnable, f, setup.where_fns);
  }
  return 0;
}

}  // namespace mch
