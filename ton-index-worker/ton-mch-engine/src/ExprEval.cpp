#include "ExprEval.h"

namespace mch {

namespace {

using Json = td::JsonValue;
using JType = td::JsonValue::Type;

const Json *jfield(const Json &e, td::Slice name) {
  if (e.type() != JType::Object) {
    return nullptr;
  }
  for (const auto &kv : e.get_object().field_values_) {
    if (kv.first == name) {
      return &kv.second;
    }
  }
  return nullptr;
}

}  // namespace

EvalResult ExprEval::eval(const Json &e) {
  const Json *kf = jfield(e, "k");
  if (kf == nullptr || kf->type() != JType::String) {
    return rt_fault("expression node missing string `k`");
  }
  const std::string k = kf->get_string().str();

  if (k == "int") {
    return rt_ok(Value::make_int(td::dec_string_to_int256(jfield(e, "v")->get_number().str())));
  }
  if (k == "str") {
    return rt_ok(Value::make_str(jfield(e, "v")->get_string().str()));
  }
  if (k == "bool") {
    return rt_ok(Value::make_bool(jfield(e, "v")->get_boolean()));
  }
  if (k == "null") {
    return rt_ok(Value::null());
  }
  if (k == "name") {
    return rt_name(env_, jfield(e, "id")->get_string().str());
  }
  if (k == "dotfield") {
    if (where_ == nullptr) {
      return rt_fault("leading-dot field is only valid inside an inline `where (expr)`");
    }
    return rt_dotfield(*where_, jfield(e, "name")->get_string().str());
  }
  if (k == "attr") {
    EvalResult of = eval(*jfield(e, "of"));
    if (of.faulted) {
      return of;
    }
    return rt_access(of.value, jfield(e, "name")->get_string().str());
  }
  if (k == "call") {
    return eval_call(e);
  }
  if (k == "lookup") {
    return eval_lookup(e);
  }
  if (k == "unary") {
    return eval_unary(e);
  }
  if (k == "bin") {
    return eval_binary(e);
  }
  if (k == "ternary") {
    EvalResult c = eval(*jfield(e, "cond"));
    if (c.faulted) {
      return c;
    }
    EvalResult cb = rt_require_bool(c.value);
    if (cb.faulted) {
      return cb;
    }
    return eval(*jfield(e, cb.value.boolean ? "then" : "else"));
  }
  if (k == "list") {
    std::vector<Value> xs;
    for (const auto &it : jfield(e, "items")->get_array()) {
      EvalResult r = eval(it);
      if (r.faulted) {
        return r;
      }
      xs.push_back(std::move(r.value));
    }
    return rt_ok(Value::make_list(std::move(xs)));
  }
  if (k == "record") {
    Value::Fields fs;
    for (const auto &f : jfield(e, "fields")->get_array()) {
      EvalResult r = eval(*jfield(f, "expr"));
      if (r.faulted) {
        return r;
      }
      fs.emplace_back(jfield(f, "name")->get_string().str(), std::move(r.value));
    }
    return rt_ok(Value::make_dict(std::move(fs)));
  }
  if (k == "mapc" || k == "quant") {
    EvalResult xs = eval(*jfield(e, "xs"));
    if (xs.faulted) {
      return xs;
    }
    const std::string var = jfield(e, "var")->get_string().str();
    const Json *body = jfield(e, "body");
    // Copy the env; the element var shadows any outer binding for the body only
    // (mirrors the Python scope frame), leaving env_ untouched.
    Env local = env_;
    ElementFn body_fn = [&](const Value &el) -> EvalResult {
      local[var] = el;
      ExprEval sub(local, lookups_, where_);
      return sub.eval(*body);
    };
    if (k == "mapc") {
      return rt_mapc(xs.value, body_fn);
    }
    return rt_quant(jfield(e, "op")->get_string().str() == "all", xs.value, body_fn);
  }
  if (k == "parse") {
    // Build-only (needs a Block target + message parsers). The interpreter runs
    // --vectors (value-only, no blocks) and sync where clauses; a parse node
    // never reaches either (build_vectors go through the generated BuildGen
    // path). Defensive fault, mirroring the where/vectors guards in codegen.
    return rt_fault("parse expression is only evaluable in a build program");
  }
  return rt_fault("unsupported expression kind '" + k + "'");
}

EvalResult ExprEval::eval_call(const Json &e) {
  std::vector<Value> args;
  for (const auto &a : jfield(e, "args")->get_array()) {
    EvalResult r = eval(a);
    if (r.faulted) {
      return r;
    }
    args.push_back(std::move(r.value));
  }
  return rt_call_builtin(jfield(e, "fn")->get_string().str(), args);
}

EvalResult ExprEval::eval_lookup(const Json &e) {
  std::vector<Value> args;
  for (const auto &a : jfield(e, "args")->get_array()) {
    EvalResult r = eval(a);
    if (r.faulted) {
      return r;
    }
    args.push_back(std::move(r.value));
  }
  return rt_lookup(lookups_, jfield(e, "name")->get_string().str(), args);
}

EvalResult ExprEval::eval_unary(const Json &e) {
  const std::string op = jfield(e, "op")->get_string().str();
  EvalResult x = eval(*jfield(e, "x"));
  if (x.faulted) {
    return x;
  }
  return op == "not" ? rt_not(x.value) : rt_neg(x.value);
}

EvalResult ExprEval::eval_binary(const Json &e) {
  const std::string op = jfield(e, "op")->get_string().str();

  // Lazy operators (control flow) — stay at the call site.
  if (op == "and" || op == "or") {
    EvalResult l = eval(*jfield(e, "l"));
    if (l.faulted) {
      return l;
    }
    EvalResult lb = rt_require_bool(l.value);
    if (lb.faulted) {
      return lb;
    }
    if (op == "and" && !lb.value.boolean) {
      return rt_ok(Value::make_bool(false));
    }
    if (op == "or" && lb.value.boolean) {
      return rt_ok(Value::make_bool(true));
    }
    EvalResult r = eval(*jfield(e, "r"));
    if (r.faulted) {
      return r;
    }
    return rt_require_bool(r.value);
  }
  if (op == "??") {
    EvalResult l = eval(*jfield(e, "l"));
    if (l.faulted || !rt_is_null(l.value)) {
      return l;
    }
    return eval(*jfield(e, "r"));
  }

  EvalResult l = eval(*jfield(e, "l"));
  if (l.faulted) {
    return l;
  }
  EvalResult r = eval(*jfield(e, "r"));
  if (r.faulted) {
    return r;
  }
  if (op == "==") return rt_eq(l.value, r.value);
  if (op == "!=") return rt_ne(l.value, r.value);
  if (op == "<") return rt_lt(l.value, r.value);
  if (op == "<=") return rt_le(l.value, r.value);
  if (op == ">") return rt_gt(l.value, r.value);
  if (op == ">=") return rt_ge(l.value, r.value);
  if (op == "+") return rt_add(l.value, r.value);
  if (op == "-") return rt_sub(l.value, r.value);
  if (op == "*") return rt_mul(l.value, r.value);
  return rt_fault("unknown binary operator '" + op + "'");
}

}  // namespace mch
