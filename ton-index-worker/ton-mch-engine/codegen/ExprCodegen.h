// Deterministic MCH IR expression-to-C++ generator.
// See docs/codegen.md for emission and runtime contracts.
#pragma once

#include "td/utils/JsonBuilder.h"

#include <string>

namespace mch_codegen {

// C++ string-literal body (escaped): \\ \" \n \t \r, plus every other control
// byte (< 0x20 and 0x7F, NUL included) as a 3-digit octal escape. An adjacent
// digit cannot extend it (the \x greedy-hex hazard). Verified by `--selftest`.
std::string cstr(const std::string &s);

// Emit one `EvalResult <fn_name>(const mch::Env&, const mch::Lookups&)` from an
// IR expression node (k-tagged JSON). Reusable for future artifact-expr modes.
std::string generate_function(const std::string &fn_name, const td::JsonValue &expr);

// Emit one `EvalResult <fn_name>(const mch::WhereEnv &w)` for a node-level
// inline `where (expr)`: `dotfield` reads the candidate block via rt_dotfield;
// `name` / `lookup` fault (a where_expr is builtin-only and unbound, mirroring
// the Python sync evaluator).
std::string generate_where_function(const std::string &fn_name, const td::JsonValue &expr);

// Emit the whole vectors_generated.cpp from a decoded {"vectors":[...]} root.
// `header` is the leading comment block (must end with a newline).
std::string generate_vectors_file(const td::JsonValue &root, const std::string &header);

// Emit the whole wheres_<suffix>_generated.cpp from a decoded IR artifact root:
// one where-function per `nodes[i]` carrying a `where_expr`, keyed by the global
// node index i (== CompiledNode::global_id), plus the `gen_wheres_<suffix>()` /
// `gen_wheres_<suffix>_source_sha()` accessors. `suffix` pairs the table with its
// document exactly like generate_builds_file (ir artifact / testbed artifact), so
// both tables link into one binary and are selected at run time by source sha.
std::string generate_wheres_file(const td::JsonValue &root, const std::string &header,
                                 const std::string &source_sha, const std::string &suffix);

// Emit a whole builds table file: one
// `BuildOutcome build_<id>_<name>(BuildEnv&)` per matcher carrying a
// build_program, plus the `gen_builds_<suffix>()` / `gen_builds_<suffix>_
// source_sha()` table accessors. Accepts an IR artifact root
// ({matchers:[...]}, id = matcher index) or a build-vectors root
// ({build_vectors:[...]}, id = case index). `suffix` gives each input
// document its own table symbols so tables from BOTH documents link into one
// binary (ir + vectors) and never need manual re-pairing.
std::string generate_builds_file(const td::JsonValue &root, const std::string &header,
                                 const std::string &source_sha, const std::string &suffix);

}  // namespace mch_codegen
