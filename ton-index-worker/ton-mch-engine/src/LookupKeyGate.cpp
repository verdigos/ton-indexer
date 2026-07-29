#include "LookupKeyGate.h"

#include "BlockTree.h"
#include "BuildRuntime.h"
#include "ClassifyCore.h"
#include "ClassifyDriver.h"
#include "IrLoader.h"
#include "ParsedBlockLookupSource.h"
#include "Value.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mch {

namespace {

// Compare on the normalized (uppercase-hex) address so a case difference is
// reported as its own signal instead of silently becoming a MISSING+SURPLUS
// pair. Case is a live hazard on the real tier-2 key path (§3.2), which is why
// it gets a line of its own here rather than being normalized away invisibly.
std::string norm_addr(const std::string &s) {
  auto n = normalize_raw_address(s);
  return n ? *n : s;
}

using KeyId = std::pair<std::string, std::string>;   // (kind, normalized addr)
using KeySet = std::map<KeyId, std::string>;         // -> the raw address as seen

std::vector<std::string> expand_fixtures(const std::vector<std::string> &paths) {
  namespace fs = std::filesystem;
  std::vector<std::string> out;
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
      out.insert(out.end(), names.begin(), names.end());
    } else {
      out.push_back(p);
    }
  }
  return out;
}

}  // namespace

int run_lookup_keys(const std::string &artifact_path, const std::vector<std::string> &paths) {
  auto r_ir = load_ir(artifact_path);
  if (r_ir.is_error()) {
    std::fprintf(stderr, "failed to load IR artifact: %s\n", r_ir.error().message().c_str());
    return 2;
  }
  LoadedIr ir = r_ir.move_as_ok();
  ClassifySetup setup = prepare_classify(ir);
  if (setup.table_missing || setup.fn_missing) {
    std::fprintf(stderr, "%s\n", setup.error.c_str());
    return 2;
  }

  std::size_t n_req = 0, n_fix = 0, n_missing = 0, n_surplus = 0, n_case = 0;
  std::size_t n_odd = 0, n_broken = 0, n_exhausted = 0;  // the exit-1 conditions
  std::size_t n_guard = 0;                               // §8.3, reviewed not asserted
  std::map<std::size_t, std::size_t> round_hist;
  std::map<std::string, std::size_t> missing_by_kind, surplus_by_kind, req_by_kind;

  namespace fs = std::filesystem;
  for (const std::string &f : expand_fixtures(paths)) {
    std::printf("=== %s\n", fs::path(f).filename().string().c_str());
    // One reference context, alive for this fixture: it backs both the fixture
    // lookup source (the batch-fetch stand-in) and the answerable-key
    // enumeration. The driver rebuilds its own context per round.
    auto r_ref = load_trace_context(f);
    if (r_ref.is_error()) {
      std::printf("ERROR: %s\n", r_ref.error().message().c_str());
      n_broken++;
      continue;
    }
    TraceContext ref = r_ref.move_as_ok();
    FixtureLookupSource fix(&ref.trace.interfaces);

    Tier2Fetch fetch = [&fix](const LookupWants &wanted) {
      std::map<std::string, Value> out;
      for (const auto &[key, ka] : wanted) {
        out.emplace(key, fix.fetch(ka.first, ka.second));  // absent -> explicit null
      }
      return out;
    };
    ParsedBlockLookupSource::InterfaceMap empty_tier1;
    auto make_ctx = [&f]() -> td::Result<std::unique_ptr<TraceContext>> {
      TRY_RESULT(c, load_trace_context(f));
      return std::make_unique<TraceContext>(std::move(c));
    };
    auto make_src = [&empty_tier1](TraceContext &, ParsedBlockLookupSource::Tier2Hook record) {
      auto src = std::make_shared<ParsedBlockLookupSource>(&empty_tier1, std::move(record));
      const ParsedBlockLookupSource::LookupStats *stats = &src->stats();
      return RoundSource{std::move(src), stats};
    };

    DriverResult dr = run_classify_fixpoint(make_ctx, make_src, ir, setup, fetch);
    if (dr.malformed) {
      std::printf("ERROR: %s\n", dr.error.c_str());
      n_broken++;
      continue;
    }
    if (dr.res.failure) {
      std::printf("TRACE_FAIL %s\n", dr.res.failure_reason.c_str());
      n_broken++;
    }
    if (dr.rounds_exhausted) {
      n_exhausted++;
    }
    // §8.3: an action DROPPED rather than published with a wrong value. Reported
    // here because this gate is the one that runs the corpus with tier 1 emptied
    // — the worst case for lookup misses, so if any fixture can reach the guard,
    // it reaches it here. Reviewed, not asserted: a non-zero is a real event, not
    // a gate failure.
    if (dr.res.wrong_value_guard > 0) {
      for (const auto &[site, n] : dr.res.wrong_value_sites) {
        std::printf("WRONG_VALUE_GUARD %s %zu\n", site.c_str(), n);
      }
      n_guard += dr.res.wrong_value_guard;
    }

    KeySet requested;
    for (const auto &[key, ka] : dr.requested) {
      const std::vector<Value> &args = ka.second;
      if (args.size() != 1 || args[0].t != VType::Str) {
        // Every registered lookup takes one Str address; anything else would
        // break the kvrocks key derivation, so it is surfaced, not skipped.
        std::printf("REQ-ODD %s %s\n", ka.first.c_str(), key.c_str());
        n_odd++;
        continue;
      }
      requested.emplace(KeyId{ka.first, norm_addr(args[0].str)}, args[0].str);
    }
    KeySet answerable;
    for (const auto &[account, ifaces] : ref.trace.interfaces) {
      for (const std::string &kind : FixtureLookupSource::kinds()) {
        if (!fix.fetch(kind, {Value::make_str(account)}).is_null()) {
          answerable.emplace(KeyId{kind, norm_addr(account)}, account);
        }
      }
    }

    for (const auto &[id, raw] : requested) {
      std::printf("REQ %s %s\n", id.first.c_str(), raw.c_str());
      req_by_kind[id.first]++;
    }
    for (const auto &[id, raw] : answerable) {
      if (requested.count(id) == 0) {
        std::printf("MISSING %s %s\n", id.first.c_str(), raw.c_str());
        missing_by_kind[id.first]++;
        n_missing++;
      }
    }
    for (const auto &[id, raw] : requested) {
      auto it = answerable.find(id);
      if (it == answerable.end()) {
        std::printf("SURPLUS %s %s\n", id.first.c_str(), raw.c_str());
        surplus_by_kind[id.first]++;
        n_surplus++;
      } else if (it->second != raw) {
        std::printf("CASE %s engine=%s fixture=%s\n", id.first.c_str(), raw.c_str(),
                    it->second.c_str());
        n_case++;
      }
    }
    std::printf("ROUNDS %zu exhausted=%d\n", dr.rounds, dr.rounds_exhausted ? 1 : 0);
    n_req += requested.size();
    n_fix += answerable.size();
    round_hist[dr.rounds]++;
  }

  auto by_kind = [](const std::map<std::string, std::size_t> &m) {
    std::string s;
    for (const auto &[kind, n] : m) {
      if (!s.empty()) s += ",";
      s += kind + "=" + std::to_string(n);
    }
    return s;
  };
  std::string rounds;
  for (const auto &[n, cnt] : round_hist) {
    if (!rounds.empty()) rounds += ",";
    rounds += std::to_string(n) + "=" + std::to_string(cnt);
  }
  std::printf("SUMMARY requested=%zu answerable=%zu missing=%zu surplus=%zu case=%zu\n", n_req,
              n_fix, n_missing, n_surplus, n_case);
  std::printf("SUMMARY req_odd=%zu broken=%zu rounds_exhausted=%zu wrong_value_guard=%zu\n", n_odd,
              n_broken, n_exhausted, n_guard);
  std::printf("SUMMARY requested_by_kind{%s}\n", by_kind(req_by_kind).c_str());
  std::printf("SUMMARY missing_by_kind{%s}\n", by_kind(missing_by_kind).c_str());
  std::printf("SUMMARY surplus_by_kind{%s}\n", by_kind(surplus_by_kind).c_str());
  std::printf("SUMMARY rounds{%s}\n", rounds.c_str());
  // MISSING/SURPLUS vs the fixture map are REVIEWED, never asserted (see the
  // header): the fixture map is Python's eagerly PREDICTED superset, so
  // containment against it cannot hold. The cross-language containment
  // assertion lives in twins/py_consumed.py, against what Python's matchers
  // actually read. What is asserted here are the properties that ARE invariants
  // of the collect pass itself.
  return (n_odd == 0 && n_case == 0 && n_broken == 0 && n_exhausted == 0) ? 0 : 1;
}

}  // namespace mch
