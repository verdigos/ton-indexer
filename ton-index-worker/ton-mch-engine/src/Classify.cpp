#include "Classify.h"

#include "ActionBuild.h"
#include "BlockTree.h"
#include "BuildDriver.h"
#include "ClassifyCore.h"
#include "BuildRuntime.h"
#include "fixtures/FixtureLoader.h"
#include "fixtures/FixtureLookupSource.h"
#include "ExprRuntime.h"
#include "fixtures/Render.h"
#include "GenMatchers.h"
#include "MsgParse.h"

#include "td/utils/base64.h"
#include "td/utils/crypto.h"

#include "common/refint.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mch {

namespace {

// Render + print a collected CoreAction from its FINAL (post-pass) state. The
// aux/flows strings (fire-time) are rendered HERE from the core's raw block/
// flow lists. The core stays rendering-agnostic.
void emit_action(const CoreAction &a) {
  std::vector<std::string> ck;
  for (Block *cb : a.produced->children_blocks) ck.push_back(block_key(cb));
  std::sort(ck.begin(), ck.end());
  std::string consumed_s = "[";
  for (std::size_t i = 0; i < ck.size(); i++) {
    if (i) consumed_s += ",";
    consumed_s += ck[i];
  }
  consumed_s += "]";

  std::string aux_s = "[";
  for (std::size_t i = 0; i < a.aux.size(); i++) {
    if (i) aux_s += ",";
    aux_s += block_key(a.aux[i]);
  }
  aux_s += "]";

  std::printf("ACTION %s anchor=%s btype=%s failed=%d broken=%d data=%s "
              "consumed=%s auto=%s\n",
              a.matcher_name.c_str(), block_key(a.anchor).c_str(), a.produced->btype.c_str(),
              a.produced->failed ? 1 : 0, a.produced->broken ? 1 : 0,
              render_value(a.produced->data).c_str(), consumed_s.c_str(),
              aux_s.c_str());
}

}  // namespace

// Dump-side render of an Action row (built lib-side by ActionBuild).
std::string render_action(const Action &a) {
  std::vector<std::pair<std::string, std::string>> f;
  f.emplace_back("id", "str:" + a.action_id);
  f.emplace_back("type", "str:" + a.type);
  f.emplace_back("success", a.success ? "true" : "false");
  f.emplace_back("start_lt", std::to_string(a.start_lt));
  f.emplace_back("end_lt", std::to_string(a.end_lt));
  f.emplace_back("start_utime", std::to_string(a.start_utime));
  f.emplace_back("end_utime", std::to_string(a.end_utime));
  f.emplace_back("mc_seqno_end", std::to_string(a.mc_seqno_end));

  auto render_strings = [](const std::vector<std::string> &values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); i++) {
      if (i) out += ",";
      out += "str:" + values[i];
    }
    return out + "]";
  };
  std::vector<std::string> th = a.tx_hashes;
  f.emplace_back("tx_hashes", render_strings(th));
  std::vector<std::string> ac = a.accounts;
  std::sort(ac.begin(), ac.end());
  f.emplace_back("accounts", render_strings(ac));
#define MCH_RENDER_ACTION_FIELD(name) \
  if (!a.name.is_null()) f.emplace_back(#name, render_value(a.name, /*omit_null_fields=*/true));
  MCH_ACTION_VALUE_FIELDS(MCH_RENDER_ACTION_FIELD)
#undef MCH_RENDER_ACTION_FIELD
  // Child rows carry a parent id; top-level rows omit it.
  if (!a.parent_action_id.empty()) {
    f.emplace_back("parent_action_id", "str:" + a.parent_action_id);
  }
  f.emplace_back("ancestor_type", render_strings(a.ancestor_type));

  std::sort(f.begin(), f.end(), [](const auto &lhs, const auto &rhs) {
    return lhs.first < rhs.first;
  });
  std::string out = "{";
  for (std::size_t i = 0; i < f.size(); i++) {
    if (i) out += ",";
    out += f[i].first + "=" + f[i].second;
  }
  return out + "}";
}

// Shared CLI dump adapter over ClassifyCore. Without an output directory it
// preserves the fixture stream; per-fixture output omits only each header line.
int run_dump(const std::vector<std::string> &paths,
             bool actions_mode, const TraceContextLoader &load_ctx,
             const LookupSourceFactory &lookup_factory, const std::string &output_dir) {
  const std::vector<CompiledMatcher> &matchers = gen_matchers_ir();
  warn_missing_artifact_parsers(matchers);

  ClassifySetup setup = prepare_classify(matchers);
  if (setup.table_missing || setup.fn_missing) {
    std::fprintf(stderr, "%s\n", setup.error.c_str());
    return 2;
  }
  for (const auto &s : setup.skips) {
    std::printf("SKIP %s reason=%s\n", s.first.c_str(), s.second.c_str());
  }

  namespace fs = std::filesystem;
  const bool per_fixture_output = actions_mode && !output_dir.empty();
  if (per_fixture_output) {
    std::error_code ec;
    fs::create_directories(output_dir, ec);
    if (ec) {
      std::fprintf(stderr, "failed to create actions output directory %s: %s\n",
                   output_dir.c_str(), ec.message().c_str());
      return 2;
    }
  }
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
    using FilePtr = std::unique_ptr<std::FILE, int (*)(std::FILE *)>;
    FilePtr fixture_output(nullptr, &std::fclose);
    std::FILE *out = stdout;
    if (per_fixture_output) {
      fs::path output_path = fs::path(output_dir) / (fs::path(f).stem().string() + ".golden");
      fixture_output.reset(std::fopen(output_path.string().c_str(), "wb"));
      if (!fixture_output) {
        std::fprintf(stderr, "failed to open actions output file %s\n",
                     output_path.string().c_str());
        return 2;
      }
      out = fixture_output.get();
    } else {
      std::fprintf(out, "=== %s\n", fs::path(f).filename().string().c_str());
    }
    auto r_ctx = load_ctx(f);
    if (r_ctx.is_error()) {
      std::fprintf(out, "ERROR: %s\n", r_ctx.error().message().c_str());
      continue;
    }
    auto ctx = r_ctx.move_as_ok();
    // Orphan warning: stderr only (stdout is the A/B channel; never fires on corpus).
    if (ctx.tree.unlinked != 0) {
      std::fprintf(stderr, "WARN %s: %zu orphan nodes unreachable from root\n",
                   fs::path(f).filename().string().c_str(), ctx.tree.unlinked);
    }
    std::shared_ptr<LookupSource> src =
        lookup_factory ? lookup_factory(ctx)
                       : std::make_shared<FixtureLookupSource>(&ctx.trace.interfaces);

    ClassifyResult res = classify_trace(ctx, matchers, setup, *src);
    if (res.failure) {
      // Per-trace exception boundary (event_processing.py): buffered, the
      // regular actions are discarded, a single TRACE_FAIL on the dump channel,
      // and the reason on stderr. In --actions mode the
      // Basic-action fallback rows follow behind a FALLBACK tag.
      // No corpus trace triggers this, the fallback path is ungated.
      std::fprintf(out, "TRACE_FAIL\n");
      std::fprintf(stderr, "TRACE_FAIL %s: %s\n", fs::path(f).filename().string().c_str(),
                   res.failure_reason.c_str());
      if (actions_mode) {
        for (const ActionRow &r : res.fallback_rows) {
          Action act;
          if (!build_action(r, act)) continue;
          std::fprintf(out, "FALLBACK anchor=%s matcher=- %s\n", block_key(r.block).c_str(),
                       render_action(act).c_str());
        }
      }
      continue;
    }
    // --actions renders the serialize_blocks ROW set (spine); --classify renders
    // the matcher FIRE list. The two sets differ (ClassifyCore.h action_rows).
    if (actions_mode) {
      // SORTED: spine order is not comparable against Python (its
      // compact_connections rebuilds next_blocks through a `set()`), and no
      // consumer of the rows depends on their order.
      std::vector<std::string> lines;
      std::unordered_map<const Block *, const std::string *> matcher_names;
      for (const CoreAction &a : res.actions) {
        matcher_names.emplace(a.produced, &a.matcher_name);
      }
      for (const ActionRow &r : res.action_rows) {
        Action act;
        if (!build_action(r, act)) continue;  // btype outside ported set
        auto matcher = matcher_names.find(r.block);
        const std::string matcher_name =
            matcher != matcher_names.end() ? *matcher->second : std::string("-");
        lines.push_back("ACTION anchor=" + block_key(r.block) + " matcher=" + matcher_name +
                        " " + render_action(act));
      }
      std::sort(lines.begin(), lines.end());
      for (const std::string &line : lines) {
        std::fprintf(out, "%s\n", line.c_str());
      }
    } else {
      for (const CoreAction &a : res.actions) {
        emit_action(a);
      }
    }
    // The trace classified cleanly and matched nothing. The synthetic `unknown`
    // row has no Block, so it prints the literal anchor `unknown` (the twin
    // prints the same) instead of a block_key.
    if (actions_mode && res.unknown_trace) {
      Action act = create_unknown_action(ctx.trace);
      std::fprintf(out, "ACTION anchor=unknown matcher=- %s\n", render_action(act).c_str());
    }
  }
  return 0;
}

int run_classify(const std::vector<std::string> &paths) {
  return run_dump(paths, /*actions_mode=*/false, load_trace_context);
}

int run_actions(const std::vector<std::string> &paths, const std::string &output_dir) {
  return run_dump(paths, /*actions_mode=*/true, load_trace_context, {}, output_dir);
}

}  // namespace mch
