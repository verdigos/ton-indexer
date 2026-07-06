// MCH IR expression -> C++ source generator (build-time codegen, C++ port).
//
// Ports the retired Python generator (indexer/events/mch/cpp_expr_emit.py) so
// the C++ build needs NO Python: the version-controlled IR JSON is the sole
// contract. Emits deterministic straight-line C++ over the ExprRuntime library
// (rt_* free functions) — the SAME runtime the interpreter uses, so a codegen
// bug cannot hide behind a divergent runtime.
//
// Codegen shape: SSA temporaries with early-return fault/null propagation
// (`EvalResult rN = rt_op(...); if (rN.faulted) return rN; Value vN = ...;`),
// lazy operators (and/or/??/ternary) as nested if/else blocks, int literals as
// parse-once function-local statics. Temporaries and literals are numbered in a
// fixed pre-order walk with stable formatting, so regeneration is byte-identical
// (CI-diffable). Byte-for-byte compatible with the Python generator's function
// bodies + table (verified at port time).
#pragma once

#include "td/utils/JsonBuilder.h"

#include <string>

namespace mch_codegen {

// C++ string-literal body (escaped): the exact escaping the Python generator
// used (\\ \" \n \t \r).
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

// Emit the whole wheres_generated.cpp from a decoded IR artifact root: one
// where-function per `nodes[i]` carrying a `where_expr`, keyed by the global
// node index i (== CompiledNode::global_id), plus the gen_wheres() table.
std::string generate_wheres_file(const td::JsonValue &root, const std::string &header,
                                 const std::string &source_sha);

// Emit the whole builds_generated.cpp (Phase B1, D2): one
// `BuildOutcome build_<id>_<name>(BuildEnv&)` per matcher carrying a
// build_program, plus the gen_builds() table. Accepts an IR artifact root
// ({matchers:[...]}, id = matcher index) or a build-vectors root
// ({build_vectors:[...]}, id = case index).
std::string generate_builds_file(const td::JsonValue &root, const std::string &header,
                                 const std::string &source_sha);

}  // namespace mch_codegen
