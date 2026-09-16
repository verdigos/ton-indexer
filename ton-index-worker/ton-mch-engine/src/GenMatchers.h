// Stable interface for the compiled matcher table. Production consumes the
// generated C++ data and does not parse an IR artifact at runtime.
#pragma once

#include "IrTables.h"

#include <vector>

namespace mch {

// Table from ir/mch_ir.json, in manifest order.
const std::vector<CompiledMatcher> &gen_matchers_ir();

// SHA-256 (hex) of the document this table was generated from. Read by the
// equivalence ctest (to prove it compared the build's own input) and by the
// startup log, so a production log line names the artifact that is compiled in.
const char *gen_matchers_ir_source_sha();

}  // namespace mch
