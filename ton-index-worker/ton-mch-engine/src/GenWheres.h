// Stable interface for the generated where_expr functions
// (src/generated/wheres_generated.cpp, produced by `mch-codegen --wheres
// <mch_ir.json>`). One function per artifact node carrying a `where_expr`,
// keyed by the ARTIFACT-GLOBAL node index (CompiledNode::global_id) — stable
// across the loader's per-matcher remap because the emitter's node ids are
// deterministic. The walker uses this table when the runner enables generated
// mode (`--match-gen`); otherwise it interprets the loader's WhereExpr trees.
// Both paths call the same ExprRuntime rt_* leaves, so they cannot diverge
// semantically — only structurally (a table generated from a DIFFERENT
// artifact is a hard error at run_match start).
#pragma once

#include "ExprRuntime.h"

#include <vector>

namespace mch {

struct GenWhere {
  int node_id;  // artifact-global node index
  EvalResult (*fn)(const WhereEnv &);
};

// The generated table, ascending node_id (artifact node order).
const std::vector<GenWhere> &gen_wheres();

// SHA-256 (hex) of the artifact the table was generated from (see
// gen_builds_source_sha in GenBuilds.h — same wrong-pairing guard).
const char *gen_wheres_source_sha();

}  // namespace mch
