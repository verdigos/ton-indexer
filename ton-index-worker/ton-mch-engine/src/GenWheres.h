// Stable interface for the generated where_expr functions (produced by
// `mch-codegen --wheres <doc> --suffix <sfx>` as a CMake build step; the outputs
// live in the build tree, not in git). One function per artifact node carrying a
// `where_expr`, keyed by the ARTIFACT-GLOBAL node index (CompiledNode::global_id)
// — stable across the loader's per-matcher remap because the emitter's node ids
// are deterministic.
//
// This is THE where evaluator of the matcher walk: prepare_classify selects the
// table by source sha and carries it in ClassifySetup, classify_trace threads it
// into matcher_match, and the walker has no other path (the ExprEval interpreter
// left mch-classify with the flip; it is now the fixtures-side conformance
// reference — see fixtures/ExprEval.h). Same per-document table split as
// GenBuilds.h:
//  - gen_wheres_ir():      from ir/mch_ir.json — production classify;
//  - gen_wheres_testbed(): OPTIONAL (-DMCH_TESTBED_IR=<artifact.json>), so a
//    phase can classify against an experimental artifact WITHOUT editing the
//    ir/mch_ir.json contract. Without the cache var this resolves to the
//    committed empty stub (TestbedStubs.cpp; its sha "" never matches).
// Generated and interpreted wheres call the same ExprRuntime rt_* leaves, so the
// two cannot diverge semantically; only a table generated from a DIFFERENT
// artifact could, and that is a hard error at setup (select_gen_wheres + the
// per-node coverage check in prepare_classify).
#pragma once

#include "ExprRuntime.h"

#include <string>
#include <vector>

namespace mch {

struct GenWhere {
  int node_id;  // artifact-global node index
  EvalResult (*fn)(const WhereEnv &);
};

// Table from ir/mch_ir.json, ascending node_id (artifact node order).
const std::vector<GenWhere> &gen_wheres_ir();
const char *gen_wheres_ir_source_sha();

// Table from ${MCH_TESTBED_IR} (or the empty stub when the var is unset).
// TODO: remove (harness): a testbed-artifact slot in the product lib, for the
// same reason as gen_builds_testbed (python-parity-audit §5.7 H4) — the table
// selection runs from ClassifyCore, so mch-classify must resolve the symbol
// itself; a fixtures-side table would lose to the stub in archive order and
// silently disable the knob.
const std::vector<GenWhere> &gen_wheres_testbed();
const char *gen_wheres_testbed_source_sha();

// The table whose source sha matches `sha` (ir first, then testbed); nullptr
// when neither matches. `which` (optional) receives "ir"/"testbed" for
// diagnostics. Mirrors select_gen_builds (GenBuilds.h).
const std::vector<GenWhere> *select_gen_wheres(const std::string &sha,
                                               const char **which = nullptr);

}  // namespace mch
