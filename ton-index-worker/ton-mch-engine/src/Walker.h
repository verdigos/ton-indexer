// MCH IR matcher WALKER — match phase only (docs/mch-engine-v2.md §4, §5, §7).
//
// A literal port of the Python reference walker (mch_ir/engine.py IrMatcher
// match methods): child-edge multi-match accumulation (§4.2), children-set
// first-match-wins over a working copy (§4.3), or first-full-subtree-wins
// (§4.4), excess/bounce absorption (§4.5), recursive frontier walk with
// exit-first-per-candidate + one-step-per-round (§4.6) and cyclic descent
// (§4.6b), parent edge (§4.7), and capture assembly with recursion frame
// gap-alignment (§5). NO build phase, NO merge — matching never mutates the
// tree, so every matcher runs over the same shared block tree with clean A/B.
//
// Determinism: every sibling-candidate choice iterates a `canonical` (stable
// sort by min_lt) copy, exactly like the Python engine's `_canonical`. The C++
// substrate already keeps next_blocks canonical, so this is a no-op for a
// single block's next_blocks and only reorders the recursion frontier's
// concatenation — matching Python.
//
// Named host predicates are NOT evaluated here: matchers that need them are
// marked `unrunnable` at load and skipped by the runner. Inline where_exprs
// ARE evaluated, composed onto the head test exactly like the Python engine
// (§4.1): after the kind test passes, the node's where_expr runs against the
// candidate block — a fault or a non-true result fails the node (never aborts
// matching). Two interchangeable evaluation paths share ExprRuntime:
//   - interpreter (default): ExprEval over the loader's WhereExpr tree;
//   - generated (pass a WhereTable): compiled functions keyed by the node's
//     artifact-global id (GenWheres.h, `mch-codegen --wheres`).
#pragma once

#include "BlockTree.h"
#include "ExprRuntime.h"
#include "IrLoader.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace mch {

// TODO: remove (harness): the GENERATED where path is harness-only. Production
// classify (ClassifyCore.cpp try_build) calls matcher_match with no WhereTable,
// so it runs the ExprEval interpreter; the only caller that passes a table is
// MatchRunner.cpp (--match-gen), and wheres_generated.cpp is compiled into
// mch-fixtures, not mch-classify. So WhereTable, the `gen` parameter, and
// eval_where's two-branch dispatch (Walker.cpp) exist for one gate mode.
// NOTE: this makes the ExprEval.h harness marker backwards — the interpreter is
// the PRODUCTION where evaluator, and codegen is the side path. Retiring
// ExprEval means wiring a WhereTable into classify_trace first, not deleting it.
//
// Generated where_expr functions keyed by artifact-global node id.
using WhereFn = EvalResult (*)(const WhereEnv &);
using WhereTable = std::unordered_map<int, WhereFn>;

// Thrown by the match recursion when the block-tree walk exceeds the depth cap
// (Walker.cpp kMaxMatchDepth). Mirrors Python's RecursionError -> per-trace
// fallback: the classify trace boundary catches it and marks the trace failed.
struct MatchDepthExceeded : std::runtime_error {
  MatchDepthExceeded() : std::runtime_error("match recursion depth exceeded") {}
};

// One assembled capture. A scalar (`one`/`opt`) has is_list=false and exactly
// one entry in `vals` (nullptr == unbound/None). A `many` capture has
// is_list=true and `vals` in assembly order (nullptr == gap-aligned None).
struct Capture {
  std::string name;
  bool is_list{false};
  std::vector<Block *> vals;
};

struct MatchResult {
  std::vector<Block *> consumed;  // traversal order, identity-deduped
  std::vector<Capture> captures;  // slot declaration order
};

// Anchor test (spec §4.1 step 1). Opcode anchors require btype == call_contract.
bool matcher_test_self(const CompiledMatcher &m, const Block *b);

// Full match attempt (test_self + walk, no build). nullopt == structural
// mismatch. Leaves the tree untouched. `gen` selects the generated where_expr
// path (must cover every where_expr node id of `m`); nullptr == interpreter.
std::optional<MatchResult> matcher_match(const CompiledMatcher &m, Block *anchor,
                                         const WhereTable *gen = nullptr);

}  // namespace mch
