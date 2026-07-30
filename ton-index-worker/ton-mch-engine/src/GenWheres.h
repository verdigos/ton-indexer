// TODO: remove (harness): this table is generated + compiled on EVERY build and
// executed by NOTHING automated — no ctest drives `--match-gen` (CMakeLists.txt
// §CTest wiring) and CI's golden run uses `--actions`, i.e. the interpreter. Its
// only caller is MatchRunner.cpp (harness exe), so production evaluates
// where_exprs through ExprEval instead (see Walker.h). Decide, don't leave it
// here: PROMOTE to production — carry the table in ClassifySetup, sha-selected
// exactly like build_fns — which is what finally lets ExprEval.cpp leave
// mch-classify and kills the WhereExpr re-encode in IrLoader.cpp:274-294; or
// delete this header together with the `mch-codegen --wheres` mode. Current
// production surface being interpreted: 4 nodes, one distinct expression
// (`not .has_internal_transfer`, matcher stonfi_v2_swap) — so the argument is
// architectural, not perf.
//
// Stable interface for the generated where_expr functions (produced by
// `mch-codegen --wheres ir/mch_ir.json` as a CMake build step; build-tree
// output, not in git). One function per artifact node carrying a `where_expr`,
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
