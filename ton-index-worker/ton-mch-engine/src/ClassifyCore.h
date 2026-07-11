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

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace mch {

// One produced action, RENDERING-AGNOSTIC: raw block pointers + fire-time
// aux/flows. The adapter renders block_key(anchor)/data/consumed/aux/flows or
// builds an Action row from `produced`. Pointers reference the caller's arena
// (TraceContext), valid for as long as the caller keeps it alive.
struct CoreAction {
  std::string matcher_name;
  Block *anchor;
  Block *produced;
  std::vector<Block *> aux;    // fire-time excess/bounce auto-append (anchor)
  std::vector<FlowOp> flows;   // fire-time flow ops (value_flow dropped §7)
};

// Buffered ATOMIC per-trace result: on a mid-trace throw the pipeline sets
// `failure` (+ reason) and returns whatever was collected — NOTHING is emitted
// by the core; the adapter decides (a dump reproduces today's TRACE_FAIL line,
// production commits only on success).
struct ClassifyResult {
  std::vector<CoreAction> actions;  // surviving post-pass, in fire order
  bool failure = false;
  std::string failure_reason;
  // Step 2c basic-actions fallback (try_classify_basic_actions): populated ONLY
  // when `failure` — a fresh leaf-only classify (no matchers) + the
  // post-processor chain, one CoreAction per surviving spine block of a basic
  // btype (call_contract / contract_deploy / ton_transfer). Production commits
  // these when classification fails. Ungated (corpus has no failing traces).
  std::vector<CoreAction> fallback_actions;
};

// Trace-independent run prep computed once from the artifact: the runnable
// matcher set, the SKIP table (name -> reason, sorted), the generated build-fn
// map, and the union of referenced lookup kinds.
struct ClassifySetup {
  std::vector<int> included;
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
