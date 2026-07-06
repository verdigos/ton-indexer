// Shared build-phase driver pieces used by `--build-smoke` (B2) and
// `--classify` (B3): canonical block keys + outcome-value rendering (dump
// format contract in BuildSmoke.h), the artifact-derived build skip table,
// capture->slot conversion, and the two-phase lookup execution (D1).
#pragma once

#include "BuildRuntime.h"
#include "IrLoader.h"
#include "Walker.h"

#include <set>
#include <string>
#include <vector>

namespace mch {

// `<btype>[#<opcode-hex8>]@<min_lt>` — the cross-language-proven key.
std::string block_key(const Block *b);

// Canonical outcome-value rendering (see BuildSmoke.h contract).
std::string render_value(const Value &v);

// Build-inclusion filter: empty == runnable end-to-end on the current C++
// surface; else the SKIP reason (match-phase preds, builder-directive,
// missing parsers / lookup kinds / host fns / shapers). The Python twin
// computes the identical set from the artifact + the same fixed lists.
std::string build_skip_reason(const CompiledMatcher &m);

// MatchResult captures -> slot values in slot-declaration order
// (scalar: Block/Null; many: List with Null gaps).
std::vector<Value> slots_from_captures(const std::vector<Capture> &captures);

// Two-phase lookup execution (D1): collect keys to FIXPOINT with
// CollectingLookupTable (a single dry pass cannot see lookup-arg-of-lookup
// keys), fetch through `src`, resume against the immutable filled table.
// Each pass runs with a fresh BuildEnv (bodies are per-run state).
BuildOutcome run_two_phase(BuildOutcome (*fn)(BuildEnv &), const Block *anchor,
                           const std::vector<Value> &slots, const Value &consumed,
                           const std::set<std::string> &kinds, const LookupSource &src);

}  // namespace mch
