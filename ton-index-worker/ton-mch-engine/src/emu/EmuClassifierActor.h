// Shadow classifier actor (em1-design §15/§16): a sibling of the emulator's
// insert path that classifies every trace emission and ANSWERS the decorated
// sink with the Action rows plus per-trace telemetry. It writes nothing itself
// — no Redis, no DB — so with the feature off (cfg.prep == nullptr, i.e.
// --mch-disable) the emulator behaves exactly as before.
#pragma once

#include "EmuTypes.h"

#include "td/actor/actor.h"

#include <array>
#include <cstddef>
#include <deque>
#include <map>
#include <string>
#include <unordered_set>

namespace mch {

// Cumulative counters; the periodic log line prints these plus the delta since
// the previous line.
struct EmuShadowStats {
  std::size_t emissions{0}, reemissions{0};
  // Gate-side outcomes: the classifier did not run (§16).
  std::size_t shed_admission{0}, shed_deadline{0}, bypassed_disabled{0};
  std::size_t classified{0}, failed{0}, convert_failed{0}, tx_limit_exceeded{0}, actions{0};
  // Subset of `actions` that came from the basic-actions fallback of a FAILED
  // classification, not from matchers: those rows are not equivalent to real
  // ones, so the totals must stay separable.
  std::size_t fallback_actions{0};
  std::size_t unported_btypes{0}, arena_refs_scrubbed{0};
  std::size_t queue_us_max{0}, classify_us_max{0}, classify_us_total{0}, latency_samples{0};
  std::size_t serialize_us_max{0}, serialize_us_total{0}, actions_blob_bytes{0};
  // Serializer sightings of a Value shape an Action field should not carry.
  std::size_t ser_float_values{0}, ser_cell_values{0}, ser_unrenderable{0};
  std::array<std::size_t, 3> by_finality{};  // root-node EmuFinality of the emission
  std::map<FailureCategory, std::size_t> by_category;
  ParsedBlockLookupSource::LookupStats lookups;  // tier1 / tier2 / miss
  Tier2Stats tier2;  // celldb reads / memo hits / budget refusals
};

class EmuClassifierActor : public td::actor::Actor {
 public:
  explicit EmuClassifierActor(EmuShadowConfig cfg) : cfg_(std::move(cfg)) {
    CHECK(cfg_.gate);  // EmuShadowConfig contract; decorate_sink relies on it too
  }

  // Request entry from the decorated sink: ALWAYS answers the promise exactly
  // once, and never throws.
  void classify(EmuTraceView view, td::Promise<EmuClassifyResult> promise);
  // The gate turned an emission away before it became a request (admission cap
  // or the sticky fail-open flag).
  void note_shed(EmuClassifyOutcome why);

  void start_up() override;
  void alarm() override;

 private:
  // Bounded LRU over trace ids: a trace is emitted 2-3 times (emulated ->
  // confirmed -> finalized) and every emission IS classified, but only the
  // first counts as new.
  bool remember_seen(const std::string &trace_id);

  EmuShadowConfig cfg_;
  EmuShadowStats stats_;
  EmuShadowStats prev_;  // snapshot at the previous log line, for the delta
  std::unordered_set<std::string> seen_traces_;
  std::deque<std::string> seen_order_;
};

}  // namespace mch
