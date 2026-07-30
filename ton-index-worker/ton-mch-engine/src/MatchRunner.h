// Match-phase dump runner (`--match` mode) for the MCH IR walker.
//
// DUMP FORMAT CONTRACT (must be reproduced byte-for-byte by the Python twin,
// twins/py_match_dump.py):
//
//   SKIP <matcher> reason=<reason>            # once, at top, sorted by matcher name
//   ...
//   === <fixture-basename>
//   MATCH <matcher> anchor=<key> consumed=[<k>,<k>,...] captures={<n>=<v>,...}
//   ...
//   === <fixture-basename>
//   ...
//
// - SKIP lines: one per unrunnable matcher (pred / where_expr dependent), sorted
//   by matcher name, emitted ONCE before any fixture (the skip set is artifact-
//   global). `reason` is e.g. `pred:name1,name2` (sorted) or `where_expr`.
// - Per fixture: a `=== <basename>` header, then MATCH lines for every
//   (block, matcher) that matches — blocks in BFS-from-root canonical order
//   (dedup by identity), matchers in priority order, gated by test_self.
// - block key = `<btype>#<opcode-hex8>@<min_lt>` for a block with an opcode,
//   else `<btype>@<min_lt>`. Uses only substrate fields proven byte-identical
//   in the A/B substrate verification (btype, opcode, min_lt) — deliberately
//   NOT tx_hash, whose cross-language string form is unverified.
// - consumed = SORTED list of block keys (set semantics per §7 — order-
//   independent). captures = per capture in slot declaration order,
//   `name=<val>`; a scalar val is a block key or `null`; a list val is
//   `[k,k,...]` in assembly order (`null` for gap-aligned None), NOT sorted.
// - A fixture that fails to load prints `ERROR: <msg>` under its header.
#pragma once

#include <string>
#include <vector>

namespace mch {

// Dump matches over each path (files or directories of *.lz4) using the
// COMPILED-IN matcher table (GenMatchers.h) and the generated where table
// (GenWheres.h) — the same match phase production classify runs.
int run_match(const std::vector<std::string> &paths);

}  // namespace mch
