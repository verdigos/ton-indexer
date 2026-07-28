// EM1 shadow classifier actor (em1-design §5): a sibling of the emulator's
// insert path that classifies a snapshot of every trace emission and LOGS the
// outcome. It writes nothing — no Redis, no DB, no promise on the sink path —
// so with the feature off (cfg.prep == nullptr, i.e. no --mch-artifact) the
// emulator behaves exactly as before.
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
  std::size_t emissions{0}, reemissions{0}, dropped{0}, snapshot_failed{0};
  std::size_t classified{0}, failed{0}, convert_failed{0}, tx_limit_exceeded{0}, actions{0};
  std::array<std::size_t, 3> by_finality{};  // root-node EmuFinality of the emission
  std::map<FailureCategory, std::size_t> by_category;
  ParsedBlockLookupSource::LookupStats lookups;  // tier1 / tier2 / miss
};

class EmuClassifierActor : public td::actor::Actor {
 public:
  explicit EmuClassifierActor(EmuShadowConfig cfg) : cfg_(std::move(cfg)) {
    CHECK(cfg_.pending);  // EmuShadowConfig contract; decorate_sink relies on it too
  }

  // Fire-and-forget entry from the decorated sink. Never throws.
  void classify(EmuTraceSnapshot snap);
  // The decorator shed a snapshot over kMaxPendingSnapshots.
  void note_dropped();
  // The decorator could not serialize an emission's cells, so it never became a
  // snapshot. Kept apart from `dropped`, which means backpressure only.
  void note_snapshot_failed();

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
