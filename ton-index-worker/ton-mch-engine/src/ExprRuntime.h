// Shared expression-runtime semantics for the MCH expression VM.
//
// This is the SINGLE implementation of every leaf operation an MCH expression
// performs — builtins, arithmetic / comparison / negation, null propagation,
// truthiness, attribute access, name resolution, lookup nodes. Both consumers
// share it, so a codegen bug can never be masked by a subtly different runtime:
//   - the tree-walking interpreter (ExprEval) — reference / fallback path;
//   - the build-time C++ code generated from IR expressions (indexer frontend
//     module mch/cpp_expr_emit.py) — production path.
//
// Fault model (unchanged from stage 4a): a fault is a DISTINCT OUTCOME, never a
// thrown exception. Every operation returns an EvalResult{faulted, value}. The
// generated code threads these with straight-line early returns
// (`if (r.faulted) return r;`); the interpreter does the same recursively.
// Null propagation is baked into each op exactly as the Python reference does it
// (arith null->null, ordering null->false, `==` structural null handling,
// builtins null-strict), so call sites never need to special-case null except
// for the lazy operators (`and`/`or`/`??`/ternary), which are control flow and
// stay at the call site.
#pragma once

#include "Value.h"

#include <map>
#include <string>
#include <vector>

namespace mch {

struct EvalResult {
  bool faulted{false};
  Value value;
  std::string message;  // fault reason (diagnostics only)
};

// Environments passed to generated expression functions and the interpreter.
using Env = std::map<std::string, Value>;
using Lookups = std::map<std::string, Value>;

// Evaluation context for a node-level inline `where (expr)` (spec §6.8,
// mch-engine-v2.md §4.1): the candidate block plus (future) slot-indexed
// captures. Both the interpreter (ExprEval with a where context) and the
// generated where-functions take this — one signature, so codegen never has to
// change when slots land: `slots[i]` will hold the capture bound to slot i
// (Null until bound), n_slots = the matcher's slot count.
struct Block;  // BlockTree.h
struct WhereEnv {
  const Block *block{nullptr};
  const Value *slots{nullptr};
  std::size_t n_slots{0};
};

// --- result helpers ---
inline EvalResult rt_ok(Value v) { return EvalResult{false, std::move(v), {}}; }
EvalResult rt_fault(const std::string &msg);
inline bool rt_is_null(const Value &v) { return v.t == VType::Null; }

// --- name / access / lookup ---
EvalResult rt_name(const Env &env, const std::string &id);
EvalResult rt_access(const Value &obj, const std::string &field);
EvalResult rt_lookup(const Lookups &lk, const std::string &name, const std::vector<Value> &args);

// Leading-dot field access inside a `where (expr)`: reads the candidate
// block's data (mirrors expr_eval._sync_eval "dotfield": null data -> null,
// dict data -> key lookup with missing-key fault, other data -> rt_access).
EvalResult rt_dotfield(const WhereEnv &w, const std::string &name);

// --- unary ---
EvalResult rt_neg(const Value &x);
EvalResult rt_not(const Value &x);
// Faults unless x is a bool; returns the bool value. Used by lazy operators and
// ternary/not conditions at the call site.
EvalResult rt_require_bool(const Value &x);

// --- binary strict operators (lazy and/or/??/ternary live at the call site) ---
EvalResult rt_eq(const Value &l, const Value &r);
EvalResult rt_ne(const Value &l, const Value &r);
EvalResult rt_lt(const Value &l, const Value &r);
EvalResult rt_le(const Value &l, const Value &r);
EvalResult rt_gt(const Value &l, const Value &r);
EvalResult rt_ge(const Value &l, const Value &r);
EvalResult rt_add(const Value &l, const Value &r);
EvalResult rt_sub(const Value &l, const Value &r);
EvalResult rt_mul(const Value &l, const Value &r);

// --- builtins (null-strict: any null argument short-circuits to null) ---
// Per-builtin cores. The interpreter dispatches through rt_call_builtin; the
// generated code (§11.4a static dispatch) calls the typed rt_builtin_<name>
// directly — no name string, no per-call args vector. ONE semantics: both
// paths run the SAME core (plan §0.2).
EvalResult rt_call_builtin(const std::string &name, const std::vector<Value> &args);
EvalResult rt_builtin_account(const Value &x);
EvalResult rt_builtin_amount(const Value &x);
EvalResult rt_builtin_asset(const Value &x);
EvalResult rt_builtin_ton_asset();
EvalResult rt_builtin_addr_none();
EvalResult rt_builtin_b64(const Value &x);
EvalResult rt_builtin_first(const Value &x);
EvalResult rt_builtin_last(const Value &x);
EvalResult rt_builtin_len(const Value &x);
EvalResult rt_builtin_sum(const Value &x);
EvalResult rt_builtin_zip(const Value &a, const Value &b);
EvalResult rt_builtin_map(const Value &a, const Value &b);

}  // namespace mch
