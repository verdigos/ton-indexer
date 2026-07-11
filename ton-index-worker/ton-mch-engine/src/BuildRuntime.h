// Build-program runtime for the MCH engine (Phase B1, docs/mch-cpp-buildprog-design.md).
//
// Ground truth: indexer/indexer/events/mch_ir/program.py (BuildProgram.run) +
// mch_ir/expr.py Evaluator. Statements are CODEGEN'd (D2): `mch-codegen
// --builds` emits one `BuildOutcome build_<matcher>(BuildEnv&)` per matcher
// with a build_program; the generated code calls ONLY the rt_* leaves here and
// in ExprRuntime, so semantics live in exactly one place.
//
// Fault model: any EvalError-equivalent (EvalResult fault) inside a build
// program is a clean match rejection (spec §10.7) — generated code early-
// returns BuildOutcome::rejected(). `reject when` firing is the same outcome.
//
// Env layout (D3): slot-indexed captures, same family as WhereEnv. `let`
// locals and parsed bodies are generated C++ locals / the env bodies map —
// capture NAMES never exist at runtime.
#pragma once

#include "ExprRuntime.h"
#include "Value.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace mch {

struct Block;  // BlockTree.h

// program.py BuildOutcome | None. rejected==true corresponds to Python's None
// (a `reject when` fired OR an EvalError surfaced); the other fields are then
// meaningless. btype empty == Python btype None (non-switch matcher: caller
// uses produces[0]).
struct BuildOutcome {
  bool is_rejected{false};
  std::string reject_reason;  // diagnostics only
  std::string btype;
  Value data;  // Dict; defaults to the empty dict (Python: data None -> {})
  bool failed{false};
  bool broken{false};

  static BuildOutcome rejected(std::string reason = {}) {
    BuildOutcome o;
    o.is_rejected = true;
    o.reject_reason = std::move(reason);
    return o;
  }
  static BuildOutcome accepted() {
    BuildOutcome o;
    o.data = Value::make_dict({});
    return o;
  }
};

// D1 lookup interface. `has` mirrors Python's registries.lookups registration
// check (unregistered kind => EvalError => rejection); `get` returns the
// fetched value (Null == unknown key, Python lookup returning None).
// Null-strict argument handling lives in rt_lookup_build, not here.
class LookupTable {
 public:
  virtual ~LookupTable() = default;
  virtual bool has(const std::string &kind) const = 0;
  virtual Value get(const std::string &kind, const std::vector<Value> &args) const = 0;
};

// B1 stub: every kind registered, every key unknown (returns null) — the
// same table shape D1's collect pass uses.
class NullLookupTable : public LookupTable {
 public:
  bool has(const std::string &) const override { return true; }
  Value get(const std::string &, const std::vector<Value> &) const override {
    return Value::null();
  }
};

// --- two-phase lookup runtime (Phase B2, D1) --------------------------------

// Canonical map key for a (kind, args) lookup call.
std::string lookup_key(const std::string &kind, const std::vector<Value> &args);

// Collect-pass table (D1): `has` covers every kind the artifact references
// (registration-first fault order — an uncovered kind faults the collect pass
// before recording anything past it); `get` serves already-fetched values and
// RECORDS misses (returning null). Re-running the build fn against this table
// until no new misses appear reaches the key fixpoint even for lookup-arg-of-
// lookup chains (a single dry pass cannot see those: the inner null
// short-circuits the outer call before its key can form).
class CollectingLookupTable : public LookupTable {
 public:
  CollectingLookupTable(const std::set<std::string> &kinds,
                        const std::map<std::string, Value> &fetched)
      : kinds_(kinds), fetched_(fetched) {
  }
  bool has(const std::string &kind) const override { return kinds_.count(kind) != 0; }
  Value get(const std::string &kind, const std::vector<Value> &args) const override;

  // Misses recorded during the pass: key -> (kind, args).
  mutable std::map<std::string, std::pair<std::string, std::vector<Value>>> misses;

 private:
  const std::set<std::string> &kinds_;
  const std::map<std::string, Value> &fetched_;
};

// Resume-pass table: immutable fetched values; missing key -> null (nullable-
// lookup semantics), `has` still covers the artifact's kind set.
class FilledLookupTable : public LookupTable {
 public:
  FilledLookupTable(std::set<std::string> kinds, std::map<std::string, Value> values)
      : kinds_(std::move(kinds)), values_(std::move(values)) {
  }
  bool has(const std::string &kind) const override { return kinds_.count(kind) != 0; }
  Value get(const std::string &kind, const std::vector<Value> &args) const override {
    auto it = values_.find(lookup_key(kind, args));
    return it != values_.end() ? it->second : Value::null();
  }

 private:
  std::set<std::string> kinds_;
  std::map<std::string, Value> values_;
};

// Fetch adapter interface (D1). Production: an actor-request round per trace;
// offline: the fixture-backed source below.
class LookupSource {
 public:
  virtual ~LookupSource() = default;
  virtual Value fetch(const std::string &kind, const std::vector<Value> &args) const = 0;
};

// Offline LookupSource over a fixture's `interfaces` section (Trace.interfaces
// — the data Python's TestInterfaceRepository serves). Kind semantics mirror
// the REAL registered Python lookup fns:
//  - jetton_wallet (registry.default_lookups): key = arg.upper();
//    account["JettonWallet"] -> Obj{balance, address, owner, jetton}; else null.
//  - nft_item (builders/dns.py RAW variant — the artifact's only consumer):
//    key = arg UN-normalized; account["NftItem"] -> Obj{address, init, index,
//    collection_address, owner_address, content}; else null.
//  - nominator_pool (builders/nominator.py): key = arg raw; the account's
//    whole interfaces Dict when it contains "NominatorPool", else null.
// Deviation note: a non-Str argument yields null here where Python's
// jetton_wallet would raise (str.upper on a non-str -> EvalError -> same
// rejection only when the spec null-checks); real specs pass envelope address
// strings only.
class FixtureLookupSource : public LookupSource {
 public:
  explicit FixtureLookupSource(const std::map<std::string, Value> *interfaces)
      : interfaces_(interfaces) {
  }
  static const std::set<std::string> &kinds();
  Value fetch(const std::string &kind, const std::vector<Value> &args) const override;

 private:
  const std::map<std::string, Value> *interfaces_;
};

// Execution environment for one generated build fn (D3).
struct BuildEnv {
  const Block *anchor{nullptr};
  const Value *slots{nullptr};  // capture values, slot-indexed (Block/Null/List)
  std::size_t n_slots{0};
  Value consumed;               // well-known `consumed`: List of Block values
  const LookupTable *lookups{nullptr};
  // program.py Evaluator.bodies: block identity -> parsed body (Null == every
  // soft-parse alternative failed). Filled by rt_parse, read by `.body`.
  std::map<const Block *, Value> bodies;
};

// `parse TARGET as T1|T2` (program.py _do_parse/_soft_parse): target null ->
// no-op; list -> parse each non-null Block element; Block -> parse. Per
// element: first type whose parser succeeds wins; parser failure tries the
// next; all fail -> body Null. An UNREGISTERED type name faults (rejection).
EvalResult rt_parse(BuildEnv &env, const Value &target, const std::vector<std::string> &types);

// Attribute access in build context: `.body` on a Block/List resolves through
// env.bodies (missing entry == "was not parse'd" fault, exactly expr_eval's
// _access with a bodies dict); every other field delegates to rt_access.
EvalResult rt_access_build(const BuildEnv &env, const Value &obj, const std::string &field);

// `lookup kind(args)` (expr.py _eval_lookup, in ITS order): unregistered kind
// -> fault FIRST; then null-strict (any null arg -> null, table untouched);
// else the table value.
EvalResult rt_lookup_build(const BuildEnv &env, const std::string &kind,
                           const std::vector<Value> &args);

// Host `fn` call (build mode only): dispatches to HostRegistry::host_fns().
// UNLIKE rt_call_builtin, arguments are NOT null-strict — they pass through to
// the fn verbatim. Unknown name -> fault; the fn's own fault propagates. The
// generated build code emits this for every `call` whose fn is not a builtin.
EvalResult rt_call_hostfn(BuildEnv &env, const std::string &name,
                          const std::vector<Value> &args);

}  // namespace mch
