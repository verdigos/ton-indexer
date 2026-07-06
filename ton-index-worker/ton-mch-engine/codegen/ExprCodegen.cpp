#include "ExprCodegen.h"

#include <stdexcept>
#include <vector>

namespace mch_codegen {

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

std::string jstr(const Json &e, const char *name) {
  const Json *f = jfield(e, td::Slice(name));
  return f != nullptr && f->type() == JType::String ? f->get_string().str() : std::string{};
}

std::string join(const std::vector<std::string> &parts, const std::string &sep) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); i++) {
    if (i) out += sep;
    out += parts[i];
  }
  return out;
}

// Ports cpp_expr_emit._Emitter: emits one function body, returning the Value
// temp name for each subexpression. The temp counter is shared across nested
// blocks so names never collide (byte-parity with the Python numbering).
class Emitter {
 public:
  // Where mode changes only the environment-dependent leaves: dotfield reads
  // the candidate block (rt_dotfield(w, ...)), name/lookup fault with the
  // Python sync evaluator's outcome (they are unbound / not evaluable there).
  explicit Emitter(bool where_mode = false) : where_mode_(where_mode) {
  }

  std::vector<std::string> lines;

  std::string emit(const Json &e) {
    const std::string k = jstr(e, "k");
    if (k == "int") {
      int i = fresh();
      line("static const td::RefInt256 lit" + num(i) +
           " = td::dec_string_to_int256(std::string(\"" + jfield(e, "v")->get_number().str() + "\"));");
      line("Value v" + num(i) + " = Value::make_int(lit" + num(i) + ");");
      return "v" + num(i);
    }
    if (k == "str") {
      int i = fresh();
      line("Value v" + num(i) + " = Value::make_str(" + cstr(jfield(e, "v")->get_string().str()) + ");");
      return "v" + num(i);
    }
    if (k == "bool") {
      int i = fresh();
      line("Value v" + num(i) + " = Value::make_bool(" +
           (jfield(e, "v")->get_boolean() ? "true" : "false") + std::string(");"));
      return "v" + num(i);
    }
    if (k == "null") {
      int i = fresh();
      line("Value v" + num(i) + " = Value::null();");
      return "v" + num(i);
    }
    if (k == "name") {
      if (where_mode_) {
        // No environment exists at test_self time (expr_eval._sync_eval).
        return bind("rt_fault(\"name '" + jstr(e, "id") + "' is not bound in a `where` clause\")");
      }
      return bind("rt_name(env, " + cstr(jstr(e, "id")) + ")");
    }
    if (k == "dotfield") {
      if (where_mode_) {
        return bind("rt_dotfield(w, " + cstr(jstr(e, "name")) + ")");
      }
      return bind("rt_fault(\"leading-dot field is only valid inside an inline `where (expr)`\")");
    }
    if (k == "attr") {
      std::string of = emit(*jfield(e, "of"));
      return bind("rt_access(" + of + ", " + cstr(jstr(e, "name")) + ")");
    }
    if (k == "call") {
      std::string argv = emit_args(*jfield(e, "args"));
      return bind("rt_call_builtin(" + cstr(jstr(e, "fn")) + ", std::vector<Value>{" + argv + "})");
    }
    if (k == "lookup") {
      if (where_mode_) {
        // Load-time rejected in a where_expr; defensive fault (sync test_self).
        return bind("rt_fault(\"lookup is not evaluable in a `where` clause\")");
      }
      std::string argv = emit_args(*jfield(e, "args"));
      return bind("rt_lookup(lk, " + cstr(jstr(e, "name")) + ", std::vector<Value>{" + argv + "})");
    }
    if (k == "unary") {
      std::string x = emit(*jfield(e, "x"));
      std::string fn = jstr(e, "op") == "not" ? "rt_not" : "rt_neg";
      return bind(fn + "(" + x + ")");
    }
    if (k == "bin") {
      return emit_bin(e);
    }
    if (k == "ternary") {
      return emit_ternary(e);
    }
    if (k == "list") {
      std::vector<std::string> items;
      for (const auto &it : jfield(e, "items")->get_array()) {
        items.push_back(emit(it));
      }
      int i = fresh();
      line("Value v" + num(i) + " = Value::make_list(std::vector<Value>{" + join(items, ", ") + "});");
      return "v" + num(i);
    }
    if (k == "record") {
      std::vector<std::string> pairs;
      for (const auto &f : jfield(e, "fields")->get_array()) {
        std::string fv = emit(*jfield(f, "expr"));
        pairs.push_back("{" + cstr(jstr(f, "name")) + ", " + fv + "}");
      }
      int i = fresh();
      line("Value v" + num(i) + " = Value::make_dict(Value::Fields{" + join(pairs, ", ") + "});");
      return "v" + num(i);
    }
    throw std::runtime_error("unsupported expression kind '" + k + "'");
  }

 private:
  bool where_mode_ = false;
  int n_ = 0;

  int fresh() { return n_++; }
  static std::string num(int i) { return std::to_string(i); }
  void line(const std::string &s) { lines.push_back(s); }

  std::string bind(const std::string &rexpr) {
    int i = fresh();
    line("EvalResult r" + num(i) + " = " + rexpr + ";");
    line("if (r" + num(i) + ".faulted) return r" + num(i) + ";");
    line("Value v" + num(i) + " = std::move(r" + num(i) + ".value);");
    return "v" + num(i);
  }

  std::string emit_args(const Json &args) {
    std::vector<std::string> vs;
    for (const auto &a : args.get_array()) {
      vs.push_back(emit(a));
    }
    return join(vs, ", ");
  }

  // Emit `node` into a fresh line buffer (for a nested if/else body), returning
  // (value_temp, lines). The temp counter stays shared.
  std::pair<std::string, std::vector<std::string>> block(const Json &node) {
    std::vector<std::string> saved;
    saved.swap(lines);
    std::string val = emit(node);
    std::vector<std::string> block_lines;
    block_lines.swap(lines);
    lines = std::move(saved);
    return {val, block_lines};
  }

  void emit_block_into(const std::vector<std::string> &block_lines) {
    for (const auto &ln : block_lines) {
      line("  " + ln);
    }
  }

  std::string emit_bin(const Json &e) {
    const std::string op = jstr(e, "op");
    if (op == "and" || op == "or") {
      return emit_logical(e, op);
    }
    if (op == "??") {
      return emit_coalesce(e);
    }
    std::string left = emit(*jfield(e, "l"));
    std::string right = emit(*jfield(e, "r"));
    return bind(strict_fn(op) + "(" + left + ", " + right + ")");
  }

  static std::string strict_fn(const std::string &op) {
    if (op == "==") return "rt_eq";
    if (op == "!=") return "rt_ne";
    if (op == "<") return "rt_lt";
    if (op == "<=") return "rt_le";
    if (op == ">") return "rt_gt";
    if (op == ">=") return "rt_ge";
    if (op == "+") return "rt_add";
    if (op == "-") return "rt_sub";
    if (op == "*") return "rt_mul";
    throw std::runtime_error("unknown binary op '" + op + "'");
  }

  std::string emit_logical(const Json &e, const std::string &op) {
    std::string left = emit(*jfield(e, "l"));
    int i = fresh();
    line("EvalResult rb" + num(i) + " = rt_require_bool(" + left + ");");
    line("if (rb" + num(i) + ".faulted) return rb" + num(i) + ";");
    line("Value v" + num(i) + ";");
    auto [right, rblock] = block(*jfield(e, "r"));
    std::string short_val = op == "and" ? "false" : "true";
    std::string cond = op == "and" ? "!rb" + num(i) + ".value.boolean" : "rb" + num(i) + ".value.boolean";
    line("if (" + cond + ") { v" + num(i) + " = Value::make_bool(" + short_val + "); } else {");
    emit_block_into(rblock);
    line("  EvalResult rr" + num(i) + " = rt_require_bool(" + right + ");");
    line("  if (rr" + num(i) + ".faulted) return rr" + num(i) + ";");
    line("  v" + num(i) + " = Value::make_bool(rr" + num(i) + ".value.boolean);");
    line("}");
    return "v" + num(i);
  }

  std::string emit_coalesce(const Json &e) {
    std::string left = emit(*jfield(e, "l"));
    int i = fresh();
    line("Value v" + num(i) + ";");
    auto [right, rblock] = block(*jfield(e, "r"));
    line("if (!rt_is_null(" + left + ")) { v" + num(i) + " = " + left + "; } else {");
    emit_block_into(rblock);
    line("  v" + num(i) + " = " + right + ";");
    line("}");
    return "v" + num(i);
  }

  std::string emit_ternary(const Json &e) {
    std::string cond = emit(*jfield(e, "cond"));
    int i = fresh();
    line("EvalResult rc" + num(i) + " = rt_require_bool(" + cond + ");");
    line("if (rc" + num(i) + ".faulted) return rc" + num(i) + ";");
    line("Value v" + num(i) + ";");
    auto [tval, tblock] = block(*jfield(e, "then"));
    auto [eval_, eblock] = block(*jfield(e, "else"));
    line("if (rc" + num(i) + ".value.boolean) {");
    emit_block_into(tblock);
    line("  v" + num(i) + " = " + tval + ";");
    line("} else {");
    emit_block_into(eblock);
    line("  v" + num(i) + " = " + eval_ + ";");
    line("}");
    return "v" + num(i);
  }
};

}  // namespace

std::string cstr(const std::string &s) {
  std::string out = "\"";
  for (char ch : s) {
    if (ch == '\\') {
      out += "\\\\";
    } else if (ch == '"') {
      out += "\\\"";
    } else if (ch == '\n') {
      out += "\\n";
    } else if (ch == '\t') {
      out += "\\t";
    } else if (ch == '\r') {
      out += "\\r";
    } else {
      out += ch;
    }
  }
  out += "\"";
  return out;
}

std::string generate_function(const std::string &fn_name, const Json &expr) {
  Emitter em;
  std::string result = em.emit(expr);
  std::vector<std::string> body;
  for (const auto &ln : em.lines) {
    body.push_back("  " + ln);
  }
  return "EvalResult " + fn_name + "(const mch::Env &env, const mch::Lookups &lk) {\n"
         "  (void)env; (void)lk;\n" +
         join(body, "\n") + "\n"
         "  return rt_ok(" + result + ");\n"
         "}\n";
}

std::string generate_where_function(const std::string &fn_name, const Json &expr) {
  Emitter em(/*where_mode=*/true);
  std::string result = em.emit(expr);
  std::vector<std::string> body;
  for (const auto &ln : em.lines) {
    body.push_back("  " + ln);
  }
  return "EvalResult " + fn_name + "(const mch::WhereEnv &w) {\n"
         "  (void)w;\n" +
         join(body, "\n") + "\n"
         "  return rt_ok(" + result + ");\n"
         "}\n";
}

std::string generate_vectors_file(const Json &root, const std::string &header) {
  const Json *vectors = jfield(root, "vectors");
  if (vectors == nullptr || vectors->type() != JType::Array) {
    throw std::runtime_error("vectors file has no `vectors` array");
  }
  const auto &arr = vectors->get_array();

  std::vector<std::string> out = {
      header, "#include \"GenVectors.h\"", "", "namespace mch {", "namespace {", ""};
  for (std::size_t i = 0; i < arr.size(); i++) {
    out.push_back(generate_function("vec_" + std::to_string(i), *jfield(arr[i], "expr")));
  }
  out.push_back("}  // namespace");
  out.push_back("");
  out.push_back("const std::vector<GenVec> &gen_vectors() {");
  out.push_back("  static const std::vector<GenVec> table = {");
  for (std::size_t i = 0; i < arr.size(); i++) {
    out.push_back("      {" + cstr(jstr(arr[i], "name")) + ", &vec_" + std::to_string(i) + "},");
  }
  out.push_back("  };");
  out.push_back("  return table;");
  out.push_back("}");
  out.push_back("");
  out.push_back("}  // namespace mch");
  out.push_back("");
  return join(out, "\n");
}

std::string generate_wheres_file(const Json &root, const std::string &header) {
  const Json *nodes = jfield(root, td::Slice("nodes"));
  if (nodes == nullptr || nodes->type() != JType::Array) {
    throw std::runtime_error("IR artifact has no `nodes` array");
  }
  const auto &arr = nodes->get_array();

  std::vector<std::size_t> where_ids;
  for (std::size_t i = 0; i < arr.size(); i++) {
    if (jfield(arr[i], td::Slice("where_expr")) != nullptr) {
      where_ids.push_back(i);
    }
  }

  std::vector<std::string> out = {
      header, "#include \"GenWheres.h\"", "", "namespace mch {", "namespace {", ""};
  for (std::size_t id : where_ids) {
    out.push_back(generate_where_function("where_" + std::to_string(id),
                                          *jfield(arr[id], td::Slice("where_expr"))));
  }
  out.push_back("}  // namespace");
  out.push_back("");
  out.push_back("const std::vector<GenWhere> &gen_wheres() {");
  out.push_back("  static const std::vector<GenWhere> table = {");
  for (std::size_t id : where_ids) {
    out.push_back("      {" + std::to_string(id) + ", &where_" + std::to_string(id) + "},");
  }
  out.push_back("  };");
  out.push_back("  return table;");
  out.push_back("}");
  out.push_back("");
  out.push_back("}  // namespace mch");
  out.push_back("");
  return join(out, "\n");
}

}  // namespace mch_codegen
