// Pure classification pipelines from schema transactions or a TraceContext to
// buffered per-trace results. They perform no I/O or rendering.
#pragma once

#include "ActionBuild.h"  // ActionRow
#include "BlockTree.h"
#include "BuildRuntime.h"
#include "IndexData.h"  // schema::Transaction
#include "IrTables.h"
#include "TraceLoader.h"
#include "Walker.h"  // WhereTable

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace mch {

struct MchEnginePrep;
class ParsedBlockLookupSource;

// Trace-terminal failure categories used by production-classifier telemetry:
//   - lookup_infra_fail:  two-phase lookup did not converge (infra error).
//   - engine_fault:       a mid-trace exception (per-trace fallback).
//   - malformed_trace:    the trace could not be loaded into an event tree
//                         (schema map / empty tree), set caller/actor-side.
// Matcher skips are setup-level facts, while clean rejection, parse-null, and
// per-lookup misses are per-attempt events. They cannot be the single terminal
// state held here and are deliberately not enumerators.
enum class FailureCategory {
  none,
  lookup_infra_fail,
  engine_fault,
  malformed_trace,
};

// Terminal per-trace outcome of the in-indexer classification stage. Exactly
// one per admitted or rejected trace input; precedence when several apply:
// history unavailable > classification state > rows. The first three carry
// rows (possibly zero) and are written; every other outcome only counts.
// Task replacement accepts persisted outcomes and broken, capped, or lone tick-tock
// skips. Missing roots and other skips remain ineligible.
//   classified / fallback / terminal_failure_empty: classify_schema_trace ran
//     (normal rows / basic-action fallback rows / failure with nothing usable).
//   timeout:              reserved; the worker keeps results of slow jobs.
//   history_unavailable:  a state read failed (tri-state lookup); a live-mode
//                         skip, a task-mode failure with retry.
//   skipped_*:            assembly-side rejections, never classified:
//     missing_root  — no legitimate root transaction;
//     broken        — the assembler published the trace as broken;
//     evicted       — payload dropped under memory pressure; never classified;
//     capped        — transaction count exceeded the per-trace cap;
//     incomplete    — payload buffer count != assembler node count, or (task mode)
//                     identity / count / end mismatch against the inventory;
//     unexpected_trace — (task mode) completed trace absent from the inventory;
//     lone_tick_tock — the trace is exactly one tick-tock transaction; python
//                      returns an empty success before matching
//                      (trace_processor.py `_is_tick_tock_transaction`).
//                      Tick-tock roots with children are classified normally.
enum class TraceOutcome {
  classified,
  fallback,
  terminal_failure_empty,
  timeout,
  history_unavailable,
  skipped_missing_root,
  skipped_broken,
  skipped_evicted,
  skipped_capped,
  skipped_incomplete,
  skipped_unexpected_trace,
  skipped_lone_tick_tock,
};

constexpr bool should_persist_outcome(TraceOutcome o) {
  return o == TraceOutcome::classified || o == TraceOutcome::fallback ||
         o == TraceOutcome::terminal_failure_empty;
}

struct TraceClassificationResult {
  schema::Trace trace;
  TraceOutcome outcome;
  std::vector<Action> rows;
  std::string failure_reason;
  bool slow{false};  // Wall-clock telemetry; never changes the outcome or rows.
  // The unknown action for the whole trace; ClassifierWriter writes it instead of
  // `rows` when empty, rejected by PostgreSQL or unrenderable.
  Action rescue;
};

struct BlockClassification {
  std::vector<TraceClassificationResult> traces;
};

struct RunReport {
  std::map<TraceOutcome, std::uint64_t> outcomes;
  std::uint64_t suppressed{0};
  std::map<std::uint32_t, BlockClassification> blocks;
};

struct LookupStatRow {
  std::string matcher;
  std::string kind;
  std::uint64_t count{0};
  std::uint64_t total_micros{0};
};

struct LookupKindStatRow {
  std::string kind;
  std::uint64_t hits{0};
  std::uint64_t misses{0};
};

struct LookupStatsSnapshot {
  std::vector<LookupStatRow> by_matcher;
  std::vector<LookupKindStatRow> by_kind;
};

void record_lookup_stat(const std::string &matcher, const std::string &kind,
                        std::uint64_t micros);
LookupStatsSnapshot take_lookup_stats();

bool outcomes_allow_replacement(const BlockClassification &bc);

constexpr const char *trace_outcome_name(TraceOutcome o) {
  switch (o) {
    case TraceOutcome::classified: return "classified";
    case TraceOutcome::fallback: return "fallback";
    case TraceOutcome::terminal_failure_empty: return "terminal_failure_empty";
    case TraceOutcome::timeout: return "timeout";
    case TraceOutcome::history_unavailable: return "history_unavailable";
    case TraceOutcome::skipped_missing_root: return "skipped_missing_root";
    case TraceOutcome::skipped_broken: return "skipped_broken";
    case TraceOutcome::skipped_evicted: return "skipped_evicted";
    case TraceOutcome::skipped_capped: return "skipped_capped";
    case TraceOutcome::skipped_incomplete: return "skipped_incomplete";
    case TraceOutcome::skipped_unexpected_trace: return "skipped_unexpected_trace";
    case TraceOutcome::skipped_lone_tick_tock: return "skipped_lone_tick_tock";
  }
  return "?";
}

// One produced action with raw block pointers. The adapter renders the final
// produced block or builds an Action row from it. Pointers reference the
// caller's arena (TraceContext), valid for as long as the caller keeps it alive.
struct CoreAction {
  std::string matcher_name;
  Block *anchor;
  Block *produced;
};

// Buffered atomic per-trace result. On a mid-trace exception the pipeline sets
// `failure` and returns what it collected; production commits only on success.
struct ClassifyResult {
  std::vector<CoreAction> actions;  // surviving post-pass, in fire order
  // Post-pass spine and recursive child rows for production serialization.
  // This differs from the matcher-fire `actions` view: this list is the
  // post-processing spine used for serialization; `actions` records fires in order.
  std::vector<ActionRow> action_rows;
  bool failure = false;
  std::string failure_reason;
  // Trace-terminal failure category (additive telemetry; none unless `failure`).
  FailureCategory failure_category = FailureCategory::none;
  // Basic-action fallback, populated only on failure. A fresh leaf-only classify
  // with no matchers runs the
  // post-processor chain, serialized the same way as `action_rows`, so every row
  // is a basic btype (call_contract / contract_deploy / ton_transfer).
  // Production commits these when classification fails. Ungated (corpus has no
  // failing traces).
  std::vector<ActionRow> fallback_rows;
  // Set when classification succeeds but neither normal nor ghost serialization
  // produces a row for a nonempty trace. The adapter emits an unknown action so
  // "classified with no recognized action" remains distinct from "not classified".
  bool unknown_trace = false;
};

struct SchemaClassifyResult {
  TraceOutcome outcome{TraceOutcome::classified};
  std::vector<Action> rows;
  std::vector<std::string> matcher_names;
  std::size_t scrubbed{0};
  std::size_t unported_btypes{0};
  std::uint64_t prepare_micros{0};
  std::uint64_t match_micros{0};
  std::uint64_t build_micros{0};
  bool unknown_row{false};
  std::string failure_reason;
  FailureCategory failure_category{FailureCategory::none};
  Action rescue;  // Built from schema transactions before classification.
};

// Trace-independent run prep computed once from the matcher table: the runnable
// matcher set, the SKIP table (name -> reason, sorted), the generated build-fn
// map, the generated where_expr table the walker evaluates, and the union of
// referenced lookup kinds.
struct ClassifySetup {
  std::vector<int> included;  // indices into the matcher table, manifest order
  std::vector<std::pair<std::string, std::string>> skips;
  std::unordered_map<int, BuildFn> build_fns;
  WhereTable where_fns;  // node global_id -> where fn (generated, or injected by the dev IR interpreter)
  std::set<std::string> kinds;
  bool table_missing = false;  // the three generated tables disagree on their source
  bool fn_missing = false;     // an included matcher has no generated fn
  std::string error;           // populated when table_missing/fn_missing
};

// Canonical BFS over the reachable graph, de-duplicated by block identity.
std::vector<Block *> gather_blocks(Block *root);

// Build the run prep from the compiled-in matcher table. No I/O. Pass the SAME
// table classify_trace will get. `included` indexes into it. On a table/fn
// mismatch, sets the *_missing flags + error and returns what it has (adapter
// reports + exits; production is startup-fatal).
ClassifySetup prepare_classify(const std::vector<CompiledMatcher> &matchers);

// Classify one trace with the matcher loop and post-processing passes. The
// pipeline allocates produced and proxy blocks from the caller-owned arena.
ClassifyResult classify_trace(TraceContext &ctx, const std::vector<CompiledMatcher> &matchers,
                              const ClassifySetup &setup, const LookupSource &src);

// Classification exceptions become terminal outcomes with no rows. Rescue
// construction precedes classification and may throw on allocation failure.
// History unavailability overrides the outcome while retaining computed rows.
// `prep` is process-lifetime const and shareable across threads.
// The interface map behind `src` is shareable iff immutable.
// `src` and its Tier2Hook capture are for one classify call on one thread only.
SchemaClassifyResult classify_schema_trace(
    const MchEnginePrep &prep, const std::string &trace_id,
    const std::vector<schema::Transaction> &txs, ParsedBlockLookupSource &src);

}  // namespace mch
