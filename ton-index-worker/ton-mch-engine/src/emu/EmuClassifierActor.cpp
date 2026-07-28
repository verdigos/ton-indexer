#include "EmuClassifierActor.h"

#include "BlockTree.h"
#include "EmuCapture.h"
#include "EmuTraceConvert.h"
#include "SchemaTraceLoader.h"

#include "td/utils/logging.h"
#include "td/utils/port/path.h"

#include <exception>
#include <set>

namespace mch {

namespace {

constexpr double kStatsIntervalSec = 10.0;
constexpr std::size_t kMaxSeenTraces = 65536;

const char *category_name(FailureCategory c) {
  switch (c) {
    case FailureCategory::none: return "none";
    case FailureCategory::lookup_infra_fail: return "lookup_infra_fail";
    case FailureCategory::engine_fault: return "engine_fault";
    case FailureCategory::malformed_trace: return "malformed_trace";
  }
  return "?";
}

// Same sequence as MchClassifierActor's per-trace path (schema_to_trace ->
// to_tree/init_block -> classify_trace over a tier-1-only lookup source), with
// the same try/catch and FailureCategory mapping. Never throws.
TraceClassifySummary classify_txs(const MchEnginePrep &prep,
                                  const ParsedBlockLookupSource::InterfaceMap &ifaces,
                                  const std::string &trace_id,
                                  const std::vector<schema::Transaction> &txs) {
  TraceClassifySummary sum;
  sum.trace_id = trace_id;
  try {
    auto r_trace = schema_to_trace(trace_id, txs);
    if (r_trace.is_error()) {
      sum.failure = true;
      sum.failure_reason = r_trace.move_as_error().message().str();
      sum.failure_category = FailureCategory::malformed_trace;
      return sum;
    }
    TraceContext ctx;
    ctx.trace = r_trace.move_as_ok();
    ctx.tree = to_tree(ctx.trace);
    if (ctx.tree.root == nullptr) {
      sum.failure = true;
      sum.failure_reason = "empty event tree";
      sum.failure_category = FailureCategory::malformed_trace;
      return sum;
    }
    ctx.root = init_block(ctx.arena, ctx.tree.root);

    ParsedBlockLookupSource src(&ifaces);  // tier-2 hook unset: no DB reads here
    ClassifyResult res = classify_trace(ctx, prep.ir, prep.setup, src);
    sum.failure = res.failure;
    sum.failure_reason = res.failure_reason;
    sum.failure_category = res.failure_category;
    sum.lookup_stats = src.stats();
    sum.action_count = res.actions.size();
    std::set<std::string> names;
    for (const CoreAction &a : res.actions) {
      names.insert(a.matcher_name);
    }
    sum.matcher_names.assign(names.begin(), names.end());
  } catch (const std::exception &e) {
    sum.failure = true;
    sum.failure_reason = std::string("exception: ") + e.what();
    sum.failure_category = FailureCategory::engine_fault;
  } catch (...) {
    sum.failure = true;
    sum.failure_reason = "unknown exception";
    sum.failure_category = FailureCategory::engine_fault;
  }
  return sum;
}

}  // namespace

void EmuClassifierActor::start_up() {
  if (!cfg_.capture_dir.empty()) {
    auto status = td::mkpath(cfg_.capture_dir + "/");
    if (status.is_error()) {
      LOG(ERROR) << "[mch-emu] capture dir '" << cfg_.capture_dir
                 << "' unusable: " << status.move_as_error();
    }
  }
  alarm_timestamp() = td::Timestamp::in(kStatsIntervalSec);
}

void EmuClassifierActor::note_dropped() {
  stats_.dropped++;
}

void EmuClassifierActor::note_snapshot_failed() {
  stats_.snapshot_failed++;
}

bool EmuClassifierActor::remember_seen(const std::string &trace_id) {
  if (!seen_traces_.insert(trace_id).second) {
    return false;
  }
  seen_order_.push_back(trace_id);
  while (seen_order_.size() > kMaxSeenTraces) {
    seen_traces_.erase(seen_order_.front());
    seen_order_.pop_front();
  }
  return true;
}

void EmuClassifierActor::classify(EmuTraceSnapshot snap) {
  // The TLB parsers throw vm::VmError, which does NOT derive from std::exception
  // (excno.hpp) and which tdactor does not catch anywhere — upstream contains it
  // with the try/catch around parse_transactions' body, and the off-pipeline seam
  // is outside that. Unguarded, one malformed cell would abort the emulator.
  try {
    stats_.emissions++;
    if (!remember_seen(snap.trace_id)) {
      stats_.reemissions++;
    }
    if (!snap.nodes.empty()) {
      stats_.by_finality[static_cast<std::size_t>(snap.nodes.front().finality)]++;
    }
    if (snap.tx_limit_exceeded) {
      stats_.tx_limit_exceeded++;
    }

    // Before conversion on purpose: a trace that fails to convert is exactly the
    // one that cannot be reproduced offline any other way.
    if (!cfg_.capture_dir.empty()) {
      auto status = write_capture(cfg_.capture_dir, snap, cfg_.global_version);
      if (status.is_error()) {
        LOG(WARNING) << "[mch-emu] trace=" << snap.trace_id
                     << " capture failed: " << status.move_as_error();
      }
    }

    auto r_txs = emu_to_schema_txs(snap, cfg_.global_version);
    if (r_txs.is_error()) {
      stats_.convert_failed++;
      LOG(WARNING) << "[mch-emu] trace=" << snap.trace_id
                   << " CONVERT FAILED: " << r_txs.move_as_error();
    } else {
      auto txs = r_txs.move_as_ok();
      TraceClassifySummary sum = classify_txs(*cfg_.prep, snap.interfaces, snap.trace_id, txs);
      stats_.actions += sum.action_count;
      stats_.lookups.tier1_hits += sum.lookup_stats.tier1_hits;
      stats_.lookups.tier2_hits += sum.lookup_stats.tier2_hits;
      stats_.lookups.misses += sum.lookup_stats.misses;
      for (const auto &[kind, n] : sum.lookup_stats.misses_by_kind) {
        stats_.lookups.misses_by_kind[kind] += n;
      }
      if (sum.failure) {
        stats_.failed++;
        stats_.by_category[sum.failure_category]++;
        LOG(WARNING) << "[mch-emu] trace=" << snap.trace_id << " FAILED ("
                     << category_name(sum.failure_category) << "): " << sum.failure_reason;
      } else {
        stats_.classified++;
        std::string names;
        for (const std::string &n : sum.matcher_names) {
          if (!names.empty()) names += ",";
          names += n;
        }
        LOG(DEBUG) << "[mch-emu] trace=" << snap.trace_id << " txs=" << txs.size()
                   << " actions=" << sum.action_count << " matchers=[" << names << "]";
      }
    }
  } catch (const vm::VmError &e) {
    stats_.convert_failed++;
    LOG(WARNING) << "[mch-emu] trace=" << snap.trace_id << " EXCEPTION (vm): " << e.get_msg();
  } catch (...) {
    stats_.convert_failed++;
    LOG(WARNING) << "[mch-emu] trace=" << snap.trace_id << " EXCEPTION";
  }

  // After the catch: every path decrements, so backpressure cannot leak.
  cfg_.pending->fetch_sub(1, std::memory_order_relaxed);
}

void EmuClassifierActor::alarm() {
  std::string cats;
  for (const auto &[cat, n] : stats_.by_category) {
    if (!cats.empty()) cats += ",";
    cats += std::string(category_name(cat)) + "=" + std::to_string(n);
  }
  // Per-kind miss attribution: with tier 2 unwired in EM1 and no dedust_pool
  // detector in the emulator, the total alone cannot separate an expected gap
  // from a real resolution failure.
  std::string miss_kinds;
  for (const auto &[kind, n] : stats_.lookups.misses_by_kind) {
    if (!miss_kinds.empty()) miss_kinds += ",";
    miss_kinds += kind + "=" + std::to_string(n);
  }
  std::size_t resolved = stats_.lookups.tier1_hits + stats_.lookups.tier2_hits;
  LOG(INFO) << "[mch-emu] emissions=" << stats_.emissions << " (+"
            << (stats_.emissions - prev_.emissions) << ")"
            << " reemissions=" << stats_.reemissions << " dropped=" << stats_.dropped
            << " snapshot_failed=" << stats_.snapshot_failed
            << " classified=" << stats_.classified << " (+"
            << (stats_.classified - prev_.classified) << ")"
            << " failed=" << stats_.failed << " convert_failed=" << stats_.convert_failed
            << " tx_limit_exceeded=" << stats_.tx_limit_exceeded
            << " actions=" << stats_.actions << " (+" << (stats_.actions - prev_.actions) << ")"
            << " finality{emulated=" << stats_.by_finality[0]
            << " confirmed=" << stats_.by_finality[1]
            << " finalized=" << stats_.by_finality[2] << "}"
            << " [" << cats << "]"
            << " lookups{tier1=" << stats_.lookups.tier1_hits
            << " tier2=" << stats_.lookups.tier2_hits << " miss=" << stats_.lookups.misses
            << " miss_by_kind={" << miss_kinds << "}"
            << " tier2_rate=" << (resolved ? (100 * stats_.lookups.tier2_hits / resolved) : 0)
            << "%}"
            << " pending=" << cfg_.pending->load(std::memory_order_relaxed);
  prev_ = stats_;
  alarm_timestamp() = td::Timestamp::in(kStatsIntervalSec);
}

}  // namespace mch
