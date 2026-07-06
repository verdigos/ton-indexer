// Conformance-vector runners for the MCH expression VM (T42).
//
// Decodes tests/mch_ir/vectors/expr_vectors.json (§12 value-encoding
// convention) and checks each vector's result against `expect` (a decoded
// value, or {"$fault": true}). Two paths, one semantics (ExprRuntime):
//   - run_vectors     — the tree-walking INTERPRETER (ExprEval);
//   - run_vectors_gen — the GENERATED C++ functions (src/generated/, produced
//                       by indexer/events/mch/cpp_expr_emit.py).
// Both print PASS/FAIL per vector + a summary; nonzero if any vector fails.
#pragma once

#include <string>

namespace mch {

int run_vectors(const std::string &path);
int run_vectors_gen(const std::string &path);

}  // namespace mch
