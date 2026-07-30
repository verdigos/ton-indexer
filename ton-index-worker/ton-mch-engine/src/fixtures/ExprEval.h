// TEST-ONLY. Part of mch-fixtures; never linked into mch-classify or any product
// binary. See ton-mch-engine/CMakeLists.txt MCH_FIXTURE_SOURCES.
//
// The CONFORMANCE REFERENCE for the expression emitter: `--vectors`
// (mch_expr_vectors ctest) runs the vector corpus through this interpreter,
// `--vectors-gen` (mch_expr_vectors_gen) runs the same corpus through generated
// code, and both are checked against the same expected values — so a codegen bug
// surfaces as a differential. Nothing else evaluates IR expression JSON any more:
// the matcher walker calls the generated where table (GenWheres.h) and build
// programs are generated too. That is why this TU left mch-classify.
// Do not extend it — new semantics go in ExprRuntime, which both paths call.
//
// Tree-walking interpreter for MCH IR expressions (docs/mch-ir-spec.md §7).
//
// It walks the IR expression encoding (k-tagged JSON) via td::JsonValue and
// delegates every leaf operation (builtins, operators, access, null propagation)
// to the shared ExprRuntime library — the SAME functions the generated C++ path
// calls, so the two paths cannot diverge semantically. Control flow
// (name/lookup/list/record + the lazy and/or/??/ternary operators) is handled
// here; the generated code emits the identical structure inline.
//
// Fault model: EvalResult-threaded, no exceptions. `evaluate` returns
// EvalResult{faulted, value}; a fault propagates via early return through the
// recursion, mirroring the generated code's straight-line early returns.
//
// Scope: expressions against vector-supplied values only — no Block/message
// envelope integration and no `body`/`parse` state (there is no walker here).
// The where-context ctor (a WhereEnv, for `dotfield`) is a leftover of the era
// when this interpreter WAS the walker's where evaluator; no caller passes one
// now (EM4 input).
#pragma once

#include "ExprRuntime.h"

#include "td/utils/JsonBuilder.h"

#include <string>

namespace mch {

class ExprEval {
 public:
  ExprEval(const Env &env, const Lookups &lookups) : env_(env), lookups_(lookups) {
  }

  // where-context evaluation: `dotfield` resolves against the candidate block
  // (rt_dotfield); without a context it faults (vectors path, build programs).
  ExprEval(const Env &env, const Lookups &lookups, const WhereEnv *where)
      : env_(env), lookups_(lookups), where_(where) {
  }

  EvalResult evaluate(const td::JsonValue &expr) { return eval(expr); }

 private:
  EvalResult eval(const td::JsonValue &e);
  EvalResult eval_call(const td::JsonValue &e);
  EvalResult eval_lookup(const td::JsonValue &e);
  EvalResult eval_unary(const td::JsonValue &e);
  EvalResult eval_binary(const td::JsonValue &e);

  const Env &env_;
  const Lookups &lookups_;
  const WhereEnv *where_{nullptr};
};

}  // namespace mch
