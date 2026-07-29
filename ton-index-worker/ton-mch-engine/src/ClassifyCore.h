// Classification CORE (plan §11 item 2 / prod-design §2): the pure pipeline —
// (TraceContext + LoadedIr + LookupSource + generated build table) → a buffered
// per-trace result. NO stdout/stderr, NO filesystem, NO fixture knowledge, NO
// rendering. The CLI dumps (Classify.cpp) and the future `mch-classify` library
// are thin adapters over this. This TU is the "core" side of the core/harness
// split; group it with the other core TUs when the library target lands.
#pragma once

#include "BlockTree.h"
#include "BuildRuntime.h"
#include "IrLoader.h"
#include "TraceLoader.h"

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
  bool failure = false;
  std::string failure_reason;
  // Trace-terminal failure category (additive telemetry; none unless `failure`).
  FailureCategory failure_category = FailureCategory::none;
  // Step 2c basic-actions fallback (try_classify_basic_actions): populated ONLY
  // when `failure` — a fresh leaf-only classify (no matchers) + the
  // post-processor chain, one CoreAction per surviving spine block of a basic
  // btype (call_contract / contract_deploy / ton_transfer). Production commits
  // these when classification fails. Ungated (corpus has no failing traces).
  std::vector<CoreAction> fallback_actions;
  // Classification SUCCEEDED but matched nothing, over a trace that has
  // transactions — trace_processor.py:44-46's `len(actions) == 0 and
  // len(trace.transactions) > 0` branch. The adapter answers it with
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

// B1 anchor prefilter: the per-trace opcode/btype inventory an included
// matcher's anchor must intersect for its outer BFS to have any chance of a
// match. `prefilter` is true only for opcode/btype anchors (the two anchor kinds
// matcher_test_self can decide from the block's own op/btype); pred-anchored
// matchers stay full-scan (their test runs a predicate, not an op/btype check).
// `opcodes`/`btypes` point into the owning CompiledMatcher's anchor sets (valid
// for the LoadedIr's lifetime).
struct AnchorFilter {
  bool prefilter = false;
  const std::unordered_set<std::uint32_t> *opcodes = nullptr;  // OpcodeSet anchor
  const std::unordered_set<std::string> *btypes = nullptr;     // BType anchor
};

// One runnable matcher: its index into LoadedIr.matchers plus the precomputed
// B1 anchor prefilter (one record, so index and filter cannot desync).
struct IncludedMatcher {
  int matcher_index = 0;
  AnchorFilter filter;
};

// Trace-independent run prep computed once from the artifact: the runnable
// matcher set, the SKIP table (name -> reason, sorted), the generated build-fn
// map, and the union of referenced lookup kinds.
struct ClassifySetup {
  std::vector<IncludedMatcher> included;  // priority order
  std::vector<std::pair<std::string, std::string>> skips;
  std::unordered_map<int, BuildOutcome (*)(BuildEnv &)> build_fns;
  std::set<std::string> kinds;
  bool table_missing = false;  // no generated build table paired with this artifact
  bool fn_missing = false;     // an included matcher has no generated fn
  std::string error;           // populated when table_missing/fn_missing
};

// Build the run prep from a loaded artifact (selects the generated build table
// by source sha). No I/O. On a table/fn mismatch, sets the *_missing flags +
// error and returns what it has (adapter reports + exits).
ClassifySetup prepare_classify(const LoadedIr &ir);

// Classify ONE trace: the matcher loop (BFS, parent-is-None guard, re-scan
// semantics) + Phase E post-process passes, buffered. `arena` is the caller's
// (usually ctx.arena) — the pipeline allocates produced/proxy blocks from it.
ClassifyResult classify_trace(TraceContext &ctx, const LoadedIr &ir,
                              const ClassifySetup &setup, const LookupSource &src);

}  // namespace mch
