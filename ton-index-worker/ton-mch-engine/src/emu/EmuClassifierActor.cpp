#include "EmuClassifierActor.h"

#include "Classify.h"
#include "EmuTraceConvert.h"
#include "RedisActionSerialize.h"

#include "td/utils/logging.h"

#include <algorithm>
#include <exception>
#include <string>
#include <utility>

namespace mch {

namespace {

constexpr double kStatsIntervalSec = 10.0;
constexpr std::size_t kMaxSeenTraces = 65536;

void record_phase_exception(EmuClassifierStats &stats, EmuClassifyResult &res,
                            const std::string &message, bool past_conversion) {
  res.failure = true;
  res.failure_reason = message;
  if (!past_conversion) {
    stats.convert_failed++;
    res.outcome = EmuClassifyOutcome::convert_failed;
    res.failure_category = FailureCategory::malformed_trace;
    res.payload.state = "convert_failed";
  } else {
    stats.failed++;
    res.outcome = EmuClassifyOutcome::classify_failed;
    res.failure_category = FailureCategory::engine_fault;
    stats.by_category[res.failure_category]++;
    res.payload.state = "fallback";
  }
}

// Build the Redis writeback payload while the view remains alive.
EmuActionPayload build_payload(const EmuTraceView &view, const std::vector<Action> &rows,
                               std::int64_t aai_score, std::string actions_blob) {
  EmuActionPayload payload;
  payload.action_count = rows.size();
  payload.aai_score = aai_score;
  payload.actions_blob = std::move(actions_blob);

  for (const Action &a : rows) {
    payload.routes.push_back(EmuActionRoute{
        .type = a.type,
        .accounts = a.accounts,
    });
    // Preserve the action ID byte-for-byte in `<trace_key>:<action_id>` members.
    const std::string member = view.trace_id + ":" + a.action_id;
    for (const std::string &account : a.accounts) {
      payload.aai.emplace_back(account, member);
    }
  }
  return payload;
}

}  // namespace

void EmuClassifierActor::start_up() {
  alarm_timestamp() = td::Timestamp::in(kStatsIntervalSec);
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

void EmuClassifierActor::classify(EmuTraceView view, std::int64_t enqueued_us,
                                  td::Promise<EmuClassifyResult> promise) {
  EmuClassifyResult res;
  res.trace_id = view.trace_id;
  // Derive payload and guard finality from the same view.
  res.payload.finality = static_cast<std::uint8_t>(view_finality(view));
  res.payload.update_seq = view.update_seq;

  const std::int64_t started_us = emu_now_us();
  res.queue_us = started_us - enqueued_us;
  if (res.queue_us > 0 && static_cast<std::size_t>(res.queue_us) > stats_.queue_us_max) {
    stats_.queue_us_max = static_cast<std::size_t>(res.queue_us);
  }

  stats_.emissions++;
  if (!remember_seen(view.trace_id)) {
    stats_.reemissions++;
  }
  if (!view.nodes.empty()) {
    stats_.by_finality[static_cast<std::size_t>(view.nodes.front().finality)]++;
  }
  if (view.tx_limit_exceeded) {
    stats_.tx_limit_exceeded++;
  }

  // Every path reaches the single response below.
  bool past_conversion = false;
  try {
    auto r_txs = emu_to_schema_txs(view);
    if (r_txs.is_error()) {
      stats_.convert_failed++;
      res.outcome = EmuClassifyOutcome::convert_failed;
      res.failure = true;
      // Conversion failures use their own counter and a non-null failure category.
      res.failure_category = FailureCategory::malformed_trace;
      res.failure_reason = r_txs.move_as_error().message().str();
      res.payload.state = "convert_failed";
      LOG(WARNING) << "[mch-emu-w" << worker_index_ << "] trace=" << view.trace_id
                   << " CONVERT FAILED: " << res.failure_reason;
    } else {
      auto txs = r_txs.move_as_ok();
      past_conversion = true;
      auto run = mch::classify(*cfg_.prep, view.trace_id, txs, *view.interfaces,
                               cfg_.tier2 ? &view.shard_states : nullptr, view.config);
      auto &classified = run.classified;
      res.failure = is_failure_outcome(classified.outcome);
      res.failure_reason = std::move(classified.failure_reason);
      res.failure_category = classified.failure_category;
      res.used_fallback = classified.outcome == TraceOutcome::fallback;
      res.skipped_payload_actions = classified.skipped_payload_actions;
      res.lookup_stats = std::move(run.lookup_stats);
      res.tier2_stats = run.tier2_stats;
      // AAI uses the minimum transaction LT as the trace-start score. Redis
      // sorted-set scores lose integer precision above 2^53.
      std::int64_t aai_score = 0;
      bool first_tx = true;
      for (const auto &tx : txs) {
        const auto lt = static_cast<std::int64_t>(tx.lt);
        aai_score = first_tx ? lt : std::min(aai_score, lt);
        first_tx = false;
      }
      // Serialize and build routes while the view keeps the cell anchor alive.
      const std::int64_t ser_started_us = emu_now_us();
      RedisActionSerializeStats serialize_stats;
      auto actions_blob = serialize_redis_actions(classified.rows, view, &serialize_stats);
      const std::uint8_t finality = res.payload.finality;
      const std::uint64_t update_seq = res.payload.update_seq;
      res.payload = build_payload(view, classified.rows, aai_score, std::move(actions_blob));
      res.payload.finality = finality;
      res.payload.update_seq = update_seq;
      res.serialize_us = emu_now_us() - ser_started_us;
      if (res.serialize_us > 0) {
        auto us = static_cast<std::size_t>(res.serialize_us);
        stats_.serialize_us_total += us;
        stats_.serialize_us_max = std::max(stats_.serialize_us_max, us);
      }
      stats_.actions_blob_bytes += res.payload.actions_blob.size();
      stats_.ser_unrenderable += serialize_stats.unrenderable;
      if (serialize_stats.unrenderable != 0) {
        LOG(WARNING) << "[mch-emu-w" << worker_index_ << "] trace=" << res.trace_id
                     << " serializer saw unrenderable=" << serialize_stats.unrenderable;
      }
      stats_.actions += classified.rows.size();
      if (res.used_fallback) {
        stats_.fallback_actions += classified.rows.size();
      }
      stats_.skipped_payload_actions += res.skipped_payload_actions;
      stats_.lookups += res.lookup_stats;
      stats_.tier2 += res.tier2_stats;
      if (res.failure) {
        stats_.failed++;
        stats_.by_category[res.failure_category]++;
        res.outcome = EmuClassifyOutcome::classify_failed;
        // Classification failures write the basic-action fallback payload.
        res.payload.state = "fallback";
        LOG(WARNING) << "[mch-emu-w" << worker_index_ << "] trace=" << view.trace_id
                     << " FAILED ("
                     << failure_category_name(res.failure_category) << "): " << res.failure_reason;
      } else {
        stats_.classified++;
        res.outcome = EmuClassifyOutcome::classified;
        // The state distinguishes a synthetic unknown row from protocol actions.
        res.payload.state = res.failure_reason.empty() ?
            (classified.unknown_row ? "unknown" : "ok") : "partial";
        if (!res.failure_reason.empty()) {
          LOG(WARNING) << "[mch-emu-w" << worker_index_ << "] trace=" << view.trace_id
                       << " classified with skipped payload actions: " << res.failure_reason;
        }
        LOG(DEBUG) << "[mch-emu-w" << worker_index_ << "] trace=" << view.trace_id
                   << " txs=" << txs.size()
                   << " actions=" << res.payload.action_count
                   << " serialize_us=" << res.serialize_us;
      }
    }
  } catch (...) {
    const std::string message = exception_message(std::current_exception());
    record_phase_exception(stats_, res, message, past_conversion);
    LOG(WARNING) << "[mch-emu-w" << worker_index_ << "] trace=" << view.trace_id
                 << " EXCEPTION: " << message;
  }

  res.classify_us = emu_now_us() - started_us;
  if (res.classify_us > 0) {
    auto us = static_cast<std::size_t>(res.classify_us);
    stats_.classify_us_total += us;
    if (us > stats_.classify_us_max) {
      stats_.classify_us_max = us;
    }
  }
  stats_.latency_samples++;

  // The single answer: after this the sink's continuation runs the insert.
  promise.set_value(std::move(res));
}

void EmuClassifierActor::alarm() {
  const std::string cats = format_counts(stats_.by_category, failure_category_name);
  // Per-kind miss attribution: the total alone cannot separate a kind with no
  // source (an expected gap) from a real resolution failure.
  const std::string miss_kinds = format_counts(stats_.lookups.misses_by_kind);
  const std::string tier2_hit_kinds = format_counts(stats_.lookups.tier2_hits_by_kind);
  std::size_t resolved = stats_.lookups.tier1_hits + stats_.lookups.tier2_hits;
  LOG(INFO) << "[mch-emu-w" << worker_index_ << "] emissions=" << stats_.emissions << " (+"
            << (stats_.emissions - prev_.emissions) << ")"
            << " reemissions=" << stats_.reemissions
            << " classified=" << stats_.classified << " (+"
            << (stats_.classified - prev_.classified) << ")"
            << " failed=" << stats_.failed << " convert_failed=" << stats_.convert_failed
            << " tx_limit_exceeded=" << stats_.tx_limit_exceeded
            << " actions=" << stats_.actions << " (+" << (stats_.actions - prev_.actions) << ")"
            << " fallback_actions=" << stats_.fallback_actions
            << " skipped_payload_actions=" << stats_.skipped_payload_actions
            << " finality{emulated=" << stats_.by_finality[0]
            << " confirmed=" << stats_.by_finality[1]
            << " finalized=" << stats_.by_finality[2] << "}"
            << " [" << cats << "]"
            << " lookups{tier1=" << stats_.lookups.tier1_hits
            << " tier2=" << stats_.lookups.tier2_hits << " miss=" << stats_.lookups.misses
            << " miss_by_kind={" << miss_kinds << "}"
            << " tier2_hit_by_kind={" << tier2_hit_kinds << "}"
            << " tier2_rate=" << (resolved ? (100 * stats_.lookups.tier2_hits / resolved) : 0)
            << "%}"
            << " tier2{fetched=" << stats_.tier2.fetched
            << " memo_hits=" << stats_.tier2.memo_hits << "}"
            << " queue_us{max=" << stats_.queue_us_max << "}"
            << " classify_us{avg="
            << (stats_.latency_samples ? stats_.classify_us_total / stats_.latency_samples : 0)
            << " max=" << stats_.classify_us_max << "}"
            << " serialize_us{avg="
            << (stats_.latency_samples ? stats_.serialize_us_total / stats_.latency_samples : 0)
            << " max=" << stats_.serialize_us_max << "}"
            << " blob_bytes=" << stats_.actions_blob_bytes
            << " ser{unrenderable=" << stats_.ser_unrenderable << "}";
  prev_ = stats_;
  alarm_timestamp() = td::Timestamp::in(kStatsIntervalSec);
}

}  // namespace mch
