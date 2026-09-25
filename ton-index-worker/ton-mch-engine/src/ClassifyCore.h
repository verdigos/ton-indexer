// Pure classification pipelines from schema transactions or a TraceContext to
// buffered per-trace results. They perform no I/O or rendering.
#pragma once

#include "ActionBuild.h"  // ActionRow
#include "BlockTree.h"
#include "BuildRuntime.h"
#include "CelldbTier2Lookup.h"
#include "IndexData.h"  // schema::Transaction
#include "IrTables.h"
#include "TraceModel.h"
#include "Walker.h"  // WhereTable

#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace mch {

struct MchEnginePrep;
class TieredLookupSource;

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

// Terminal result for each trace input. `skipped_incomplete` also covers task
// identity, count, or end mismatches; lone tick-tock applies only to one transaction.
// Broken, capped, and lone-tick-tock skips still permit task replacement.
enum class TraceOutcome {
  classified,
  fallback,
  terminal_failure_empty,
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

constexpr bool is_failure_outcome(TraceOutcome o) {
  return o == TraceOutcome::fallback || o == TraceOutcome::terminal_failure_empty;
}

constexpr const char *failure_category_name(FailureCategory category) {
  switch (category) {
    case FailureCategory::none: return "none";
    case FailureCategory::lookup_infra_fail: return "lookup_infra_fail";
    case FailureCategory::engine_fault: return "engine_fault";
    case FailureCategory::malformed_trace: return "malformed_trace";
  }
  return "?";
}

template <typename Key, typename Count, typename Name>
std::string format_counts(const std::map<Key, Count> &counts, Name name) {
  std::string out;
  for (const auto &[key, count] : counts) {
    if (!out.empty()) out += ",";
    out += std::string(name(key)) + "=" + std::to_string(count);
  }
  return out;
}

template <typename Count>
std::string format_counts(const std::map<std::string, Count> &counts) {
  return format_counts(counts, [](const std::string &key) -> const std::string & { return key; });
}

struct ClassifyStats {
  TieredLookupSource::LookupStats lookups;
  Tier2Stats tier2;

  ClassifyStats &operator+=(const ClassifyStats &other) {
    lookups += other.lookups;
    tier2 += other.tier2;
    return *this;
  }

  std::string format() const;
};

struct TraceClassificationResult {
  schema::Trace trace;
  TraceOutcome outcome;
  std::vector<Action> rows;
  std::string failure_reason;
  bool slow{false};  // Wall-clock telemetry; never changes the outcome or rows.
  // The unknown action for the whole trace; ClassifierWriter writes it instead of
  // `rows` when empty, rejected by PostgreSQL or unrenderable.
  Action rescue;
  ClassifyStats stats;
  std::size_t skipped_payload_actions{0};
};

struct BlockClassification {
  std::vector<TraceClassificationResult> traces;
};

struct RunReport {
  std::map<TraceOutcome, std::uint64_t> outcomes;
  std::uint64_t suppressed{0};
  std::map<std::uint32_t, BlockClassification> blocks;
};

bool outcomes_allow_replacement(const BlockClassification &bc);

constexpr const char *trace_outcome_name(TraceOutcome o) {
  switch (o) {
    case TraceOutcome::classified: return "classified";
    case TraceOutcome::fallback: return "fallback";
    case TraceOutcome::terminal_failure_empty: return "terminal_failure_empty";
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
  // Post-pass spine and recursive child rows for serialization. `actions`
  // instead records matcher fires in order.
  std::vector<ActionRow> action_rows;
  bool failure = false;
  std::string failure_reason;
  // Trace-terminal failure category (additive telemetry; none unless `failure`).
  FailureCategory failure_category = FailureCategory::none;
  // Basic-action fallback, populated only on failure. A fresh leaf-only classify
  // with no matchers runs the
  // post-processor chain, serialized the same way as `action_rows`, so every row
  // is a basic btype (call_contract / contract_deploy / ton_transfer).
  // Production commits these when classification fails.
  std::vector<ActionRow> fallback_rows;
  // Set when classification succeeds but neither normal nor ghost serialization
  // produces a row for a nonempty trace. The adapter emits an unknown action so
  // "classified with no recognized action" remains distinct from "not classified".
  bool unknown_trace = false;
};

struct SchemaClassifyResult {
  TraceOutcome outcome{TraceOutcome::classified};
  std::vector<Action> rows;
  std::size_t skipped_payload_actions{0};
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
  WhereTable where_fns;  // node global_id -> where fn
  std::set<std::string> kinds;
  std::string error;
};

// Build the run prep from the compiled-in matcher table. No I/O. Pass the SAME
// table classify_trace will get. `included` indexes into it. On a table/fn
// mismatch, sets error and returns what it has (adapter reports + exits;
// production is startup-fatal).
ClassifySetup prepare_classify(const std::vector<CompiledMatcher> &matchers);

// Classify one trace with the matcher loop and post-processing passes. The
// pipeline allocates produced and proxy blocks from the caller-owned arena.
ClassifyResult classify_trace(TraceContext &ctx, const std::vector<CompiledMatcher> &matchers,
                              const ClassifySetup &setup, const LookupSource &src);

// Rescue allocation failures escape. Engine failures return an empty terminal
// outcome; invalid actions and tier-2 read errors affect only their action or lookup.
// `prep` is process-lifetime const and shareable across threads.
// The interface map behind `src` is shareable iff immutable.
// `src` and its Tier2Hook capture are for one classify call on one thread only.
SchemaClassifyResult classify_schema_trace(
    const MchEnginePrep &prep, const std::string &trace_id,
    const std::vector<schema::Transaction> &txs, TieredLookupSource &src);

std::string exception_message(std::exception_ptr error);
std::string exception_message(std::exception_ptr error, bool prefix_exception);

}  // namespace mch
