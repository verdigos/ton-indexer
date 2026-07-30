// MCH matcher table -> C++ source generator (build-time codegen).
//
// The counterpart of ExprCodegen for the MATCH phase: where that emits code for
// expressions, this emits DATA — the CompiledMatcher table load_ir() used to
// build at every process start.
//
// It does not re-parse the artifact. mch-codegen links the loader (mch-ir-load)
// and hands the already-compiled table to this printer, so there is exactly ONE
// implementation of the artifact->table compile step in the tree and no second
// parser that could drift from it. What remains to verify is the printer, which
// the `mch_ir_equivalence` ctest does by comparing the emitted table against
// load_ir()'s output field by field.
#pragma once

#include "fixtures/IrLoader.h"

#include <string>

namespace mch_codegen {

// Emit the whole matchers_<suffix>_generated.cpp from a loaded artifact: one
// builder function per matcher plus the `gen_matchers_<suffix>()` /
// `gen_matchers_<suffix>_source_sha()` accessors. `header` is the leading comment
// block (must end with a newline). Emission is deterministic — unordered
// containers are printed in sorted order — so regeneration is byte-identical.
std::string generate_matchers_file(const mch::LoadedIr &ir, const std::string &header,
                                   const std::string &suffix);

}  // namespace mch_codegen
