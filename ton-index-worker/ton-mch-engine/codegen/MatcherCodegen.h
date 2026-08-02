// Deterministic MCH matcher-table-to-C++ generator.
// It prints the loader's compiled table instead of parsing the artifact again.
#pragma once

#include "fixtures/IrLoader.h"

#include <string>

namespace mch_codegen {

// Emit the whole matchers_<suffix>_generated.cpp from a loaded artifact: one
// builder function per matcher plus the `gen_matchers_<suffix>()` /
// `gen_matchers_<suffix>_source_sha()` accessors. `header` is the leading comment
// block (must end with a newline). Unordered containers are sorted so
// regeneration is byte-identical.
std::string generate_matchers_file(const mch::LoadedIr &ir, const std::string &header,
                                   const std::string &suffix);

}  // namespace mch_codegen
