// Build-phase smoke runner (`--build-smoke`, Phase B2 gate).
//
// End-to-end over REAL fixtures: match-phase walk (interpreter where_exprs)
// -> capture slots -> TWO-PHASE lookup (collect-to-fixpoint with
// CollectingLookupTable, fetch through FixtureLookupSource over the fixture's
// `interfaces` section, resume with FilledLookupTable) -> GENERATED build fn
// -> BuildOutcome dump. NO merge / no produced-block reinsertion / no
// auto-append — that is Phase B3; this isolates build+lookup parity.
//
// DUMP FORMAT CONTRACT (Python twin: py_build_smoke.py — NOT COMMITTED; this
// mode has no twin in twins/, so its dump is regression-checked by eye only):
//   SKIP <matcher> reason=...          # once, sorted: match-phase preds OR
//                                      # build deps outside the C++ surface
//   === <fixture-basename>
//   BUILD <matcher> anchor=<block_key> rejected
//   BUILD <matcher> anchor=<block_key> btype=<bt|-> failed=0|1 broken=0|1
//         data={k=v,...}                             # one line, data keys sorted
// Value rendering: null | true/false | <dec int> | amount:<dec> | str:<s> |
// <raw account>|addr_none | asset:TON|asset:<raw> | b64:<base64> |
// cell:<HASH> | obj{k=v,..} | {k=v,..} (both key-sorted) | [v,..] |
// block:<block_key>.
//
// Runs the COMPILED-IN tables (GenMatchers.h / GenBuilds.h), same as production.
#pragma once

#include <string>
#include <vector>

namespace mch {

int run_build_smoke(const std::vector<std::string> &paths);

}  // namespace mch
