// TODO: remove (harness): the gate that guards the retirement of runtime IR
// loading. It compares the COMPILED-IN matcher table (GenMatchers.h) against the
// table load_ir() builds from the same document, field by field. mch-codegen
// prints what load_ir() produced, so what this actually gates is the printer —
// the one step between the version-controlled artifact and the table production
// runs. Keep it as long as ir/mch_ir.json is a hand-edited contract.
//
// Hermetic: input is the committed artifact, no fixture corpus, no Python.
#pragma once

#include <string>

namespace mch {

// Compare gen_matchers_ir() with load_ir(artifact_path). Prints the first
// divergence (matcher, field, both values) and returns 1; 0 on full equality.
int run_ir_equivalence(const std::string &artifact_path);

}  // namespace mch
