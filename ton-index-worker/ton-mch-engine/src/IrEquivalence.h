// Hermetic consistency check between the compiled matcher table and load_ir().
// It validates the generated printer output while ir/mch_ir.json remains a
// hand-edited contract. No fixture corpus or Python runtime is required.
#pragma once

#include <string>

namespace mch {

// Compare gen_matchers_ir() with load_ir(artifact_path). Prints the first
// divergence (matcher, field, both values) and returns 1; 0 on full equality.
int run_ir_equivalence(const std::string &artifact_path);

}  // namespace mch
