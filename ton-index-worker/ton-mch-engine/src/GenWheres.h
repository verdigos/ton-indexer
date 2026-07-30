// Stable interface for the generated where_expr functions (produced by
// `mch-codegen --wheres <doc> --suffix <sfx>` as a CMake build step; the outputs
// live in the build tree, not in git). One function per artifact node carrying a
// `where_expr`, keyed by the ARTIFACT-GLOBAL node index (CompiledNode::global_id)
// — stable across the per-matcher remap because the emitter's node ids are
// deterministic.
//
// This is THE where evaluator of the matcher walk: prepare_classify loads the
// table into ClassifySetup, classify_trace threads it into matcher_match, and the
// walker has no other path (the ExprEval interpreter left mch-classify with the
// where-table flip; it is now the fixtures-side conformance reference — see
// fixtures/ExprEval.h). Generated and interpreted wheres call the same
// ExprRuntime rt_* leaves, so the two cannot diverge semantically.
//
// Generated from MCH_CLASSIFY_ARTIFACT, the same compile-time document choice
// that feeds the matcher and build tables (see GenBuilds.h).
#pragma once

#include "ExprRuntime.h"

#include <vector>

namespace mch {

struct GenWhere {
  int node_id;  // artifact-global node index
  EvalResult (*fn)(const WhereEnv &);
};

// Table from MCH_CLASSIFY_ARTIFACT, ascending node_id (artifact node order).
const std::vector<GenWhere> &gen_wheres_ir();

// SHA-256 (hex) of the document this table was generated from — the value
// prepare_classify compares against the matcher and build tables'.
const char *gen_wheres_ir_source_sha();

}  // namespace mch
