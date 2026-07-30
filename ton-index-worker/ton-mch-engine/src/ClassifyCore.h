// Classification CORE (plan §11 item 2 / prod-design §2): the pure pipeline —
// (TraceContext + the compiled matcher table + LookupSource + generated build
// table) → a buffered
// per-trace result. NO stdout/stderr, NO filesystem, NO fixture knowledge, NO
// rendering. The CLI dumps (Classify.cpp) and the future `mch-classify` library
// are thin adapters over this. This TU is the "core" side of the core/harness
// split; group it with the other core TUs when the library target lands.
#pragma once

#include "BlockTree.h"
#include "BuildRuntime.h"
#include "IrTables.h"
#include "TraceLoader.h"
#include "Walker.h"  // WhereTable

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace mch {

// Failure/skip taxonomy (additive, Phase G slice 3 review). A shared vocabulary
// for WHY a matcher/trace did not produce actions; used for shadow-mode
// telemetry only — rendering-agnostic, no dump/twin sees it, existing gates stay
// byte-stable. TRACE-TERMINAL reasons only:
//   - lookup_infra_fail:  two-phase lookup did not converge (infra error).
//   - engine_fault:       a mid-trace exception (Python's per-trace fallback).
//   - malformed_trace:    the trace could not be loaded into an event tree
//                         (schema map / empty tree) — set caller/actor-side.
// Deliberately NOT enumerators: matcher_skip (a setup-level fact, read from
// ClassifySetup.skips, never a per-trace outcome) and clean_reject / parse_null /
// the per-lookup tier misses (per-(block, matcher, lookup) events on the hot
// path — counted in ParsedBlockLookupSource::LookupStats, or left uncounted).
// None of those can be the single trace-terminal value this field holds, so an
// enumerator for them would be a state the type never reaches.
enum class FailureCategory {
  none,
  lookup_infra_fail,
  engine_fault,
  malformed_trace,
};

// One produced action, RENDERING-AGNOSTIC: raw block pointers + fire-time aux.
// The adapter renders block_key(anchor)/data/consumed/aux or builds an Action
// row from `produced`. Pointers reference the caller's arena (TraceContext),
// valid for as long as the caller keeps it alive.
struct CoreAction {
  std::string matcher_name;
  Block *anchor;
  Block *produced;
  std::vector<Block *> aux;    // fire-time excess/bounce auto-append (anchor)
};

// Buffered ATOMIC per-trace result: on a mid-trace throw the pipeline sets
// `failure` (+ reason) and returns whatever was collected — NOTHING is emitted
// by the core; the adapter decides (a dump reproduces today's TRACE_FAIL line,
// production commits only on success).
struct ClassifyResult {
  std::vector<CoreAction> actions;  // surviving post-pass, in fire order
  // The blocks serialize_blocks (block_tree_serializer.py:1641-1681) turns into
  // rows: the POST-PASS SPINE, in spine order. NOT the same set as `actions`,
  // which is the matcher FIRE list:
  //   - a composite a later matcher consumed is off the spine — its row belongs
  //     to the outer block, so Python never emits it (the fire list still has
  //     it, which is what --classify wants to show);
  //   - every leaf no matcher consumed is a BASIC row (ton_transfer /
  //     call_contract / contract_deploy, the last promoted onto the spine by
  //     unwind_deployments) — 143 of the 165 corpus fixtures have at least one.
  // THIS is the production / `--actions` row set; `actions` stays the --classify
  // view. A row needs nothing from the fire (no aux, no matcher name), so it is
  // just the block.
  //
  // One more source feeds it: when the main spine serializes NOTHING, the
  // ghost-external fallback (GhostExternal.h) may replace the whole set with
  // rows synthesized from a failed wallet external's unsent messages. Those
  // never appear in `actions` — they are not matcher fires, and --classify has
  // no fallback view.
  std::vector<Block *> action_rows;
  bool failure = false;
  std::string failure_reason;
  // Trace-terminal failure category (additive telemetry; none unless `failure`).
  FailureCategory failure_category = FailureCategory::none;
  // Step 2c basic-actions fallback (try_classify_basic_actions): populated ONLY
  // when `failure` — a fresh leaf-only classify (no matchers) + the
  // post-processor chain, serialized the same way as `action_rows`, so every row
  // is a basic btype (call_contract / contract_deploy / ton_transfer).
  // Production commits these when classification fails. Ungated (corpus has no
  // failing traces).
  std::vector<Block *> fallback_rows;
  // Classification SUCCEEDED but serialized nothing AND the ghost-external
  // fallback found nothing either, over a trace that has transactions —
  // trace_processor.py:44-46's `len(actions) == 0 and
  // len(trace.transactions) > 0` branch, i.e. `action_rows` empty (Python tests
  // serialize_blocks' output, not the fire list). The adapter answers it with
  // create_unknown_action(ctx.trace); that row has no Block, so it cannot be a
  // CoreAction and the core only raises the flag.
  //
  // Why it matters beyond parity: ton-index-go reads `Classified` as "the
  // `actions` field exists", so an EMPTY array is reported to API consumers as
  // a successful classification (emulated/models.go:838-844). Without this row,
  // "we looked and found nothing classifiable" is indistinguishable from "this
  // trace was never classified".
  bool unknown_trace = false;
};

// Trace-independent run prep computed once from the matcher table: the runnable
// matcher set, the SKIP table (name -> reason, sorted), the generated build-fn
// map, the generated where_expr table the walker evaluates, and the union of
// referenced lookup kinds.
struct ClassifySetup {
  std::vector<int> included;  // indices into the matcher table, priority order
  std::vector<std::pair<std::string, std::string>> skips;
  std::unordered_map<int, BuildOutcome (*)(BuildEnv &)> build_fns;
  WhereTable where_fns;  // node global_id -> generated where fn (GenWheres.h)
  std::set<std::string> kinds;
  bool table_missing = false;  // the three generated tables disagree on their source
  bool fn_missing = false;     // an included matcher has no generated fn
  std::string error;           // populated when table_missing/fn_missing
};

// Build the run prep from the compiled-in matcher table. No I/O. Pass the SAME
// table classify_trace will get — `included` indexes into it. On a table/fn
// mismatch, sets the *_missing flags + error and returns what it has (adapter
// reports + exits; production is startup-fatal).
ClassifySetup prepare_classify(const std::vector<CompiledMatcher> &matchers);

// Classify ONE trace: the matcher loop (BFS, parent-is-None guard, re-scan
// semantics) + Phase E post-process passes, buffered. `arena` is the caller's
// (usually ctx.arena) — the pipeline allocates produced/proxy blocks from it.
ClassifyResult classify_trace(TraceContext &ctx, const std::vector<CompiledMatcher> &matchers,
                              const ClassifySetup &setup, const LookupSource &src);

}  // namespace mch
