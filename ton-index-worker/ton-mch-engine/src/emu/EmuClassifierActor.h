// EM1.5 shadow classifier actor (em1-design §15/§16): a sibling of the
// emulator's insert path that classifies every trace emission and ANSWERS the
// decorated sink with the Action rows plus per-trace telemetry. It writes
// nothing itself — no Redis, no DB (EM2 moves the rows into the insert) — so
// with the feature off (cfg.prep == nullptr, i.e. no --mch-artifact) the
// emulator behaves exactly as before.
#pragma once

#include "ClassifyDriver.h"  // ClassifyFixpoint, LookupWants
#include "EmuTypes.h"

#include "td/actor/actor.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mch {

// Cumulative counters; the periodic log line prints these plus the delta since
// the previous line.
struct EmuShadowStats {
  std::size_t emissions{0}, reemissions{0};
  // Gate-side outcomes: the classifier did not run (§16). `shed_oversize` is the
  // §5 work cap and is the only one of these that can fire on a HEALTHY
  // classifier — the others all mean saturation or a wedge.
  std::size_t shed_admission{0}, shed_deadline{0}, shed_oversize{0}, bypassed_disabled{0};
  std::size_t classified{0}, failed{0}, convert_failed{0}, tx_limit_exceeded{0}, actions{0};
  // Subset of `actions` that came from the basic-actions fallback of a FAILED
  // classification, not from matchers: those rows are not equivalent to real
  // ones, so the totals must stay separable.
  std::size_t fallback_actions{0};
  std::size_t unported_btypes{0}, arena_refs_scrubbed{0};
  std::size_t queue_us_max{0}, classify_us_max{0}, classify_us_total{0}, latency_samples{0};
  // W3 (§3): the write-back's own cost, kept apart from classify_us so a
  // regression is attributable to the half it came from.
  std::size_t serialize_us_max{0}, serialize_us_total{0}, actions_blob_bytes{0};
  // ActionSerializeStats, accumulated. All three SHOULD stay zero: a Float or a
  // Cell reaching an Action means a fill started producing a shape §2.7 says
  // never reaches one, and `unrenderable` means a value went out as nil.
  std::size_t ser_float_values{0}, ser_cell_values{0}, ser_unrenderable{0};
  std::array<std::size_t, 3> by_finality{};  // root-node EmuFinality of the emission
  std::map<FailureCategory, std::size_t> by_category;
  ParsedBlockLookupSource::LookupStats lookups;  // tier1 / tier2 / miss
};

// Tier-2 counters (em2-tier2-design §8.4), a SIBLING of EmuShadowStats so
// ParsedBlockLookupSource::LookupStats — the tier attribution the engine itself
// keeps — stays untouched. DECLARED HERE, WIRED IN W4: the fetch/breaker/sweep
// counters need the async lookup path, and `wrong_value_guard` needs the reject
// site's kind + matcher name plumbed out of the host layer (its reject already
// landed in W1: HostTonco.cpp answers a router-wallet miss with a null record
// instead of a TON asset).
struct EmuTier2Stats {
  std::size_t requested{0}, served{0}, absent{0};  // asked / non-null / null
  std::size_t fetch_errors{0}, breaker_trips{0}, breaker_open{0};
  std::size_t rounds{0}, rounds_exhausted{0}, deadline_hits{0}, swept{0}, shed_suspended{0};
  // §8.3 action-scoped rejects. Deliberately NOT a FailureCategory: that field
  // holds trace-terminal reasons only (ClassifyCore.h), and a clean reject
  // leaves the trace successful.
  std::size_t wrong_value_guard{0};
  std::size_t fetch_us_total{0}, fetch_us_max{0}, batches{0}, suspended_max{0};
  std::map<std::string, std::size_t> requested_by_kind;
};

// One emission's state, from the moment its transactions are converted until
// its promise is answered (§5.1). HEAP-owned and never moved once built: the
// fixpoint's context/source builders hold references to `view.interfaces` and
// `txs`, which live right here, so the whole object has to stay put for as long
// as the fixpoint can run another pass.
struct PendingClassify {
  // Monotonic, and the ONLY handle the outstanding fetch carries. Not a
  // pointer: a response for a state the sweep already committed then finds no
  // entry and is a clean no-op instead of a use-after-free.
  std::uint64_t id{0};
  EmuTraceView view;
  // The expensive TLB parse (§4.1), hoisted out of the round loop and paid
  // once per emission however many passes run.
  std::vector<schema::Transaction> txs;
  std::unique_ptr<ClassifyFixpoint> fx;
  EmuClassifyResult res;
  std::int64_t started_us{0};
  std::int64_t deadline_us{0};
  std::int64_t suspended_us{0};  // when the in-flight fetch was issued
  td::Promise<EmuClassifyResult> promise;
};

class EmuClassifierActor : public td::actor::Actor {
 public:
  explicit EmuClassifierActor(EmuShadowConfig cfg) : cfg_(std::move(cfg)) {
    CHECK(cfg_.gate);  // EmuShadowConfig contract; decorate_sink relies on it too
  }
  ~EmuClassifierActor() override = default;

  // Request entry from the decorated sink: ALWAYS answers the promise exactly
  // once, and never throws. With tier 2 wired the answer may come from a LATER
  // mailbox turn — between the fetch and the resume this returns and the
  // mailbox keeps draining, which is the entire point: a network round must not
  // become a head-of-line stall.
  void classify(EmuTraceView view, td::Promise<EmuClassifyResult> promise);
  // The gate turned an emission away before it became a request (admission cap
  // or the sticky fail-open flag).
  void note_shed(EmuClassifyOutcome why);

  // Tier-2 resume paths (§5.1). Both are no-ops for an id the sweep already
  // committed. `on_lookup_failed` is byte-for-byte the same classification as
  // an all-null batch (§8.1); it differs only in feeding the circuit breaker.
  void on_lookup_batch(std::uint64_t id, std::map<std::string, Value> values);
  void on_lookup_failed(std::uint64_t id, td::Status why);

  void start_up() override;
  void alarm() override;

 protected:
  // The per-emission fixpoint. Virtual for ONE reason: the S7 gate
  // (--tier2-suspend-test) substitutes a SCRIPTED fixpoint, which is what lets
  // the suspension loop be gated with no store, no artifact and no fixture
  // corpus — fixture traces carry parsed transactions but no transaction CELLS,
  // so this actor's own converter cannot be driven from the corpus, and ctest's
  // gate set is hermetic by convention. Production never overrides it.
  virtual std::unique_ptr<ClassifyFixpoint> make_fixpoint(PendingClassify &st);

 private:
  // Bounded LRU over trace ids: a trace is emitted 2-3 times (emulated ->
  // confirmed -> finalized) and every emission IS classified, but only the
  // first counts as new.
  bool remember_seen(const std::string &trace_id);

  // --- the suspension loop (§5) ---
  // One classify pass, then the suspend-or-commit decision.
  void run_pass(std::unique_ptr<PendingClassify> st);
  // Hand the round's keys to the fetch and return to the mailbox.
  void suspend(std::unique_ptr<PendingClassify> st, LookupWants wanted);
  // Publish the fixpoint's retained pass: build the Action rows, then finish.
  // Every stop reason lands here (converged, cap, deadline, breaker,
  // suspended-cap shed, sweep).
  void commit_pass(std::unique_ptr<PendingClassify> st);
  // Serialize the rows, fold the per-emission telemetry, answer.
  void finish(std::unique_ptr<PendingClassify> st, const std::vector<Action> &rows,
              const std::vector<std::string> &matcher_names, std::size_t scrubbed);
  // The single answer path: latency accounting, the promise, and the release of
  // this trace's ordering slot. Answers exactly once.
  void answer(std::unique_ptr<PendingClassify> st, bool record_latency);
  // §5.2: a second emission of a SUSPENDED trace waits for it rather than
  // racing it.
  void start_or_queue(std::unique_ptr<PendingClassify> st);
  void release_trace(const std::string &trace_id);
  std::unique_ptr<PendingClassify> take_pending(std::uint64_t id);
  // §5.5: commit every suspended state whose response is past due.
  void sweep_pending(std::int64_t now_us);
  // Make sure the next alarm is within one sweep interval.
  void arm_sweep();
  // §5.6.
  bool breaker_open(std::int64_t now_us);
  void note_fetch_ok();
  void note_fetch_failed(std::int64_t now_us);
  void log_stats();

  EmuShadowConfig cfg_;
  EmuShadowStats stats_;
  EmuShadowStats prev_;  // snapshot at the previous log line, for the delta
  EmuTier2Stats tier2_;
  std::unordered_set<std::string> seen_traces_;
  std::deque<std::string> seen_order_;

  // SUSPENDED emissions only — an emission classifying right now is on the
  // stack, not here. Bounded by kMaxSuspended (§5.7).
  std::map<std::uint64_t, std::unique_ptr<PendingClassify>> pending_;
  std::uint64_t next_pending_id_{1};
  // §5.2 secondary ordering mechanism (the primary is the write-back's
  // actions_finality guard, which also covers reordering this cannot see —
  // across inserter actors and across process restarts).
  std::unordered_set<std::string> inflight_traces_;
  std::unordered_map<std::string, std::deque<std::unique_ptr<PendingClassify>>> queued_;

  // §5.6 breaker: consecutive fetch failures, and the instant the open state
  // expires (0 == closed).
  std::size_t breaker_failures_{0};
  std::int64_t breaker_until_us_{0};
  std::int64_t last_stats_us_{0};
};

}  // namespace mch
