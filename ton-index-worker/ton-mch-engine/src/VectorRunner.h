// Conformance-vector runners for the MCH expression VM.
//
// Decodes ir/expr_vectors.json with the tagged value encoding and checks each
// vector's result against `expect` (a decoded value, or {"$fault": true}). Two
// paths, one semantics (ExprRuntime):
//   - run_vectors: the tree-walking interpreter (ExprEval);
//   - run_vectors_gen: generated C++ functions emitted into the build tree
//                       by `mch-codegen --vectors ir/expr_vectors.json`.
// Both print PASS/FAIL per vector + a summary; nonzero if any vector fails.
#pragma once

#include "Value.h"

#include "td/utils/JsonBuilder.h"

#include <string>

namespace mch {

int run_vectors(const std::string &path);
int run_vectors_gen(const std::string &path);

// Tagged value encoding to Value ($amount/$account/$asset_*/$bytes_b64/
// $cell_b64/$obj/$list/$dict/plain). Shared with the build-vector runner.
Value decode_vector_value(const td::JsonValue &e);

}  // namespace mch
