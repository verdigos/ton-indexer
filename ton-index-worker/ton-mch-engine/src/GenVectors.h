// Stable interface for the generated expression-vector functions
// (src/generated/vectors_generated.cpp, produced by
// indexer/events/mch/cpp_expr_emit.py). Consumed by `--vectors-gen`.
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
