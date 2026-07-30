// Stable interface for the generated MATCHER TABLE (produced by
// `mch-codegen --matchers <doc> --suffix <sfx>` as a CMake build step; the output
// lives in the build tree, not in git).
//
// This is the match phase's input: the whole CompiledMatcher table of the IR
// document, compiled into the binary as C++ data. Production never parses an IR
// artifact at run time — the document is a BUILD input, so the table and the
// binary cannot disagree by construction.
//
// The emitter parses with the same load_ir() the `mch_ir_equivalence` ctest uses
// as its reference, so the only thing between artifact and table is the printer,
// and that ctest is what gates the printer.
#pragma once

#include "IrTables.h"

#include <vector>

namespace mch {

// Table from ir/mch_ir.json, in priority order (registration_order, then a
// stable sort by priority) — the order the classify loop runs matchers in.
const std::vector<CompiledMatcher> &gen_matchers_ir();

// SHA-256 (hex) of the document this table was generated from. Read by the
// equivalence ctest (to prove it compared the build's own input) and by the
// startup log, so a production log line names the artifact that is compiled in.
const char *gen_matchers_ir_source_sha();

}  // namespace mch
