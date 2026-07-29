// Trace-level tier-2 fixpoint driver (em2-tier2-design §4, W1).
//
// The engine's own two-phase lookup fixpoint (BuildDriver.cpp:235-254) is scoped
// to ONE (matcher, block) build invocation and fetches synchronously, so there
// is no seam where a trace-wide key set can be collected and fetched in one
// batched round. This TU is that seam:
//
//   classify -> collect the keys tier 1 could not serve -> batch fetch ->
//   rebuild the context -> re-classify, capped at kMaxTier2Rounds, committing
//   the LAST pass.
//
// Why a re-classify and not a resume (§4.2): classify_trace MUTATES its inputs —
// it merges consumed blocks into produced ones, reparents in the post-process
// passes and grows the spine in unwind_deployments — so a second call on the
// same TraceContext would run over a half-eaten tree. Every round therefore
// rebuilds the context (O(#tx)); the expensive TLB parse stays hoisted out in
// the caller, which passes already-converted transactions in.
//
// With no Tier2Fetch — production today, and every offline gate — the collected
// keys are never fetched, round 0 commits, and the result is byte-identical to
// the pre-driver single-pass code: the recording hook returns null exactly like
// the unset std::function it replaces (ParsedBlockLookupSource.cpp:149), so the
// tier-2 miss accounting is unchanged as well.
#pragma once

#include "BlockTree.h"    // TraceContext
#include "ClassifyCore.h"  // ClassifyResult, ClassifySetup
#include "IrLoader.h"      // LoadedIr
#include "ParsedBlockLookupSource.h"

#include "IndexData.h"  // schema::Transaction (tondb-scanner, PUBLIC)

#include "td/utils/Status.h"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mch {

// Hard cap on classify passes per emission (§4.4). Rounds > 2 are reachable: a
// tier-2 hit can unblock a matcher that previously rejected, its btype folds
// into the anchor inventory, and a later btype-anchored matcher runs its own
// lookups. On exhaustion the driver COMMITS the last pass and flags it — unlike
// the inner loop, which throws (BuildDriver.cpp:252-254); throwing here would
// make a trace that classifies fine today classify worse once tier-2 is on.
// Raised 4 -> 6 in W4 on W1's evidence: forcing the multi-round path over the
// 165-fixture corpus with tier 1 EMPTIED (the worst case — production tier 1
// answers in-trace accounts in-pass) converged at 1/2/3/4 passes for
// 61/85/17/**2** fixtures. Two traces landing exactly on a cap whose overflow
// degrades SILENTLY to last-pass-commit is no headroom; both are multi-DEX
// chains (coffee + dedust + stonfi_v2), where each stage's btype unblocks the
// next stage's lookups. `rounds_exhausted` stays watched in every live run.
inline constexpr std::size_t kMaxTier2Rounds = 6;

// The keys one pass wanted but could not get: lookup_key -> (kind, args).
using LookupWants = std::map<std::string, std::pair<std::string, std::vector<Value>>>;

// Synchronous batched tier-2 fetch (W3/W4 make the actor side asynchronous
// around it). CONTRACT (§6.5): every requested key comes back, a miss as an
// explicit null — the driver stores an explicit null for anything unanswered so
// a permanently-absent key cannot make the fixpoint spin to the cap.
using Tier2Fetch = std::function<std::map<std::string, Value>(const LookupWants &)>;

// A round's lookup source plus, when it is a ParsedBlockLookupSource, a pointer
// to its telemetry (owned by `src`, read right after the pass). Only the FINAL
// pass's stats are kept (§8.4) — each pass builds a fresh source, so folding
// intermediate passes in would double-count.
struct RoundSource {
  std::shared_ptr<LookupSource> src;
  const ParsedBlockLookupSource::LookupStats *stats = nullptr;
};

// Builds a FRESH context for a round (§4.2). Called once per pass.
using ContextBuilder = std::function<td::Result<std::unique_ptr<TraceContext>>()>;

// Builds the round's lookup source. `record` is the driver's recording tier-2
// hook; a builder that has its own tier-2 resolver (the offline gate's fixture
// fallback) may ignore it, in which case nothing is collected and round 0
// commits.
using RoundSourceBuilder =
    std::function<RoundSource(TraceContext &, ParsedBlockLookupSource::Tier2Hook)>;

struct DriverResult {
  // The COMMITTED pass's context. It owns the arena the CoreAction block
  // pointers reference, so it must outlive any use of `res`.
  std::unique_ptr<TraceContext> ctx;
  ClassifyResult res;
  ParsedBlockLookupSource::LookupStats lookup_stats;  // final pass only
  // Union over all rounds of the keys the recording hook collected — this
  // trace's COLLECT-PASS KEY SET (§10 W2's gate subject; W4 folds its per-kind
  // histogram into EmuTier2Stats::requested_by_kind). Each key appears once:
  // the hook records only what `fetched` does not already carry.
  LookupWants requested;
  std::size_t rounds = 0;         // classify passes executed
  bool rounds_exhausted = false;  // committed at the cap with keys still wanted
  bool malformed = false;         // the context could not be built
  std::string error;              // reason, when `malformed`
};

// The fixpoint, STEP-WISE. The synchronous entry points below wrap this; an
// ACTOR drives it directly, because it cannot block inside a Tier2Fetch callback
// — it has to hand control back to its mailbox between the pass and the response
// (§5.1). One `ClassifyFixpoint` is exactly the per-trace state a suspended
// emission owns.
//
//   ClassifyFixpoint fx(...);
//   while (fx.step() == FixpointStep::wants_lookups) {
//     if (cannot_fetch) { fx.commit_current(); break; }
//     fx.feed(<answers for fx.wanted()>);        // the suspension point
//   }
//   DriverResult out = fx.take_result();
//
// Never catches: a throw from a builder or from the engine propagates to the
// caller's existing per-trace exception boundary unchanged.
enum class FixpointStep {
  committed,      // the pass just run is final; take_result() is valid
  wants_lookups,  // wanted() must be answered, or commit_current() called
};

class ClassifyFixpoint {
 public:
  ClassifyFixpoint(ContextBuilder make_ctx, RoundSourceBuilder make_src, const LoadedIr &ir,
                   const ClassifySetup &setup, std::size_t max_rounds = kMaxTier2Rounds)
      : make_ctx_(std::move(make_ctx))
      , make_src_(std::move(make_src))
      , ir_(ir)
      , setup_(setup)
      , max_rounds_(max_rounds) {
  }

  // Rebuild the context and run ONE classify pass. Commits when the pass wanted
  // nothing, when the context could not be built, or at the round cap.
  FixpointStep step();

  // The keys the last pass could not serve.
  const LookupWants &wanted() const { return wanted_; }

  // Merge a batch of answers. Anything left unanswered becomes an explicit null,
  // so the next pass cannot ask for it again and the fixpoint terminates. A
  // caller that cannot fetch AT ALL must use commit_current() instead of
  // feeding an empty map, which would spend a round to no purpose.
  void feed(const std::map<std::string, Value> &values);

  // Give up fetching and commit the pass already run — what makes the
  // tier-2-less path (production today, every offline gate) commit round 0.
  void commit_current();

  bool committed() const { return committed_; }
  std::size_t rounds() const { return rounds_; }
  DriverResult take_result() { return std::move(result_); }

 private:
  void commit();

  ContextBuilder make_ctx_;
  RoundSourceBuilder make_src_;
  const LoadedIr &ir_;
  const ClassifySetup &setup_;
  std::size_t max_rounds_;

  // Monotonically growing across rounds: key -> value, a miss stored as an
  // explicit null. Spans one emission only — there is no cross-trace cache
  // (§7), because a stale balance would be a WRONG value, not a miss.
  std::map<std::string, Value> fetched_;
  LookupWants wanted_;
  std::size_t rounds_{0};
  bool committed_{false};
  // The pass just run, held until it is either committed or superseded.
  std::unique_ptr<TraceContext> pass_ctx_;
  ClassifyResult pass_res_;
  DriverResult result_;
};

// The PRODUCTION fixpoint over a trace's already-converted schema transactions
// + the in-block interface map: `schema_to_trace -> to_tree/init_block` per
// round, classified through a ParsedBlockLookupSource carrying the recording
// hook. `trace_id`, `txs`, `ifaces`, `ir` and `setup` MUST outlive the returned
// object — the builders hold references to them. The SUSPENDING actor keeps the
// first three next to the fixpoint in one per-trace state object (§5.1); the
// synchronous entry below keeps them on its own stack.
std::unique_ptr<ClassifyFixpoint> make_trace_fixpoint(
    const std::string &trace_id, const std::vector<schema::Transaction> &txs,
    const ParsedBlockLookupSource::InterfaceMap &ifaces, const LoadedIr &ir,
    const ClassifySetup &setup, std::size_t max_rounds = kMaxTier2Rounds);

// Synchronous convenience wrapper: drives a ClassifyFixpoint to completion with
// a blocking fetch (or none). This is the shape W1-W3's gates exercise, so it
// stays the regression net for the step-wise machinery underneath.
DriverResult run_classify_fixpoint(const ContextBuilder &make_ctx,
                                   const RoundSourceBuilder &make_src, const LoadedIr &ir,
                                   const ClassifySetup &setup, const Tier2Fetch &fetch = {},
                                   std::size_t max_rounds = kMaxTier2Rounds);

// Production entry: a trace's already-converted schema transactions + the
// in-block interface map, classified through a ParsedBlockLookupSource carrying
// the recording hook. Reproduces the actors' former inline sequence
// (schema_to_trace -> to_tree -> init_block -> classify_trace) exactly,
// including the two malformed cases and their reason strings.
DriverResult classify_trace_fixpoint(const std::string &trace_id,
                                     const std::vector<schema::Transaction> &txs,
                                     const ParsedBlockLookupSource::InterfaceMap &ifaces,
                                     const LoadedIr &ir, const ClassifySetup &setup,
                                     const Tier2Fetch &fetch = {},
                                     std::size_t max_rounds = kMaxTier2Rounds);

}  // namespace mch
