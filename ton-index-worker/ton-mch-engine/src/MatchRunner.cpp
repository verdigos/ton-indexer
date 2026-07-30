#include "MatchRunner.h"

#include "BlockTree.h"
#include "fixtures/Render.h"  // shared block_key (§11 item 2 dedupe)
#include "GenWheres.h"
#include "fixtures/FixtureLoader.h"
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

// block_key is the shared Block dump-key helper (fixtures/Render.h) — §11 item 2
// dedupe removed the local copy that used to live here.

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

void dump_fixture(const LoadedIr &ir, const std::string &path, const WhereTable *gen) {
  auto r_ctx = load_trace_context(path);
  if (r_ctx.is_error()) {
    std::printf("ERROR: %s\n", r_ctx.error().message().c_str());
    return;
  }
  auto ctx = r_ctx.move_as_ok();
  for (Block *block : gather_blocks(ctx.root)) {
    for (const CompiledMatcher &m : ir.matchers) {
      if (m.unrunnable) {
        continue;
      }
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

int run_match(const std::string &artifact_path, const std::vector<std::string> &paths,
              bool use_gen) {
  auto r_ir = load_ir(artifact_path);
  if (r_ir.is_error()) {
    std::fprintf(stderr, "failed to load IR artifact: %s\n", r_ir.error().message().c_str());
    return 2;
  }
  LoadedIr ir = r_ir.move_as_ok();

  WhereTable gen_table;
  const WhereTable *gen = nullptr;
  if (use_gen) {
    if (ir.source_sha256 != gen_wheres_source_sha()) {
      std::fprintf(stderr,
                   "generated where table was built from a DIFFERENT artifact (table sha %s, "
                   "loaded %s) — the CMake build regenerates it from ir/mch_ir.json; rebuild "
                   "ton-mch-engine or pass that artifact\n",
                   gen_wheres_source_sha(), ir.source_sha256.c_str());
      return 2;
    }
    for (const GenWhere &g : gen_wheres()) {
      gen_table.emplace(g.node_id, g.fn);
    }
    // The table must have been generated from THIS artifact: every runnable
    // matcher's where_expr node needs a compiled function.
    for (const CompiledMatcher &m : ir.matchers) {
      if (m.unrunnable) {
        continue;
      }
      for (const CompiledNode &n : m.nodes) {
        if (n.has_where_expr && gen_table.find(n.global_id) == gen_table.end()) {
          std::fprintf(stderr,
                       "generated where table has no function for node %d (matcher %s) — "
                       "rebuild ton-mch-engine (CMake regenerates it from ir/mch_ir.json)\n",
                       n.global_id, m.name.c_str());
          return 2;
        }
      }
    }
    gen = &gen_table;
  }

  // SKIP lines (once, sorted by matcher name).
  std::vector<std::pair<std::string, std::string>> skips;
  for (const CompiledMatcher &m : ir.matchers) {
    if (m.unrunnable) {
      skips.emplace_back(m.name, m.skip_reason);
    }
  }
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
    dump_fixture(ir, f, gen);
  }
  return 0;
}

}  // namespace mch
