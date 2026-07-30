// TODO: remove (harness): TEST-ONLY, like fixtures/GenBuildsVectors.h. Compiled
// into mch-fixtures (CMakeLists.txt MCH_GENERATED_TEST_SOURCES), never into
// mch-classify or a product binary. It exists so `--vectors-gen` can be diffed
// against the `--vectors` interpreter run; the `mch-codegen --vectors` mode
// exists only to produce it. That differential IS load-bearing (it is what
// proves the emitter against ExprEval), so this dies only with the vectors
// corpus itself — not as part of retiring runtime IR.
//
// Stable interface for the generated expression-vector functions (produced by
// `mch-codegen --vectors ir/expr_vectors.json` as a CMake build step;
// build-tree output, not in git). Consumed by `--vectors-gen`.
#pragma once

#include "ExprRuntime.h"

#include <vector>

namespace mch {

struct GenVec {
  const char *name;
  EvalResult (*fn)(const Env &, const Lookups &);
};

// The generated table, in expr_vectors.json order (index-aligned with the JSON).
const std::vector<GenVec> &gen_vectors();

}  // namespace mch
