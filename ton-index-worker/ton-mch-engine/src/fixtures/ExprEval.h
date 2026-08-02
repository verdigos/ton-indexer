// Test-only tree-walking interpreter used as the expression-codegen conformance
// reference. Leaf semantics live in ExprRuntime; this class owns JSON traversal
// and lazy control flow. See docs/codegen.md and docs/value-model.md.
#pragma once

#include "ExprRuntime.h"

#include "td/utils/JsonBuilder.h"

#include <string>

namespace mch {

class ExprEval {
 public:
  ExprEval(const Env &env, const Lookups &lookups) : env_(env), lookups_(lookups) {
  }

  // With a where context, `dotfield` resolves against the candidate block
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
