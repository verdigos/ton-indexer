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
// matching). The evaluator is the GENERATED table (`mch-codegen --wheres`,
// GenWheres.h): compiled functions keyed by the node's artifact-global id,
// selected by source sha and coverage-checked at setup. There is no second
// path — the ExprEval interpreter left this library with the flip and now
// serves only as the emitter's conformance reference (fixtures/ExprEval.h).
#pragma once

#include "BlockTree.h"
#include "ExprRuntime.h"
#include "IrTables.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace mch {

// Generated where_expr functions keyed by artifact-global node id. Built once
// per run by prepare_classify (from gen_wheres_ir) and carried in
// ClassifySetup; matcher_match takes it by reference, so a walk without a table
// is not representable.
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
// mismatch. Leaves the tree untouched. `gen` must cover every where_expr node id
// of `m` (guaranteed by prepare_classify's coverage check); an uncovered node
// fails its head test rather than aborting the walk.
std::optional<MatchResult> matcher_match(const CompiledMatcher &m, Block *anchor,
                                         const WhereTable &gen);

}  // namespace mch
