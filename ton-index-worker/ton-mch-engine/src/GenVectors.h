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
