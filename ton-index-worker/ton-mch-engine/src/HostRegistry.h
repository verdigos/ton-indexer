// Host bindings ported from the Python registries (Phase D).
//
// Predicates (`predicate NAME` in specs; Python impls live in
// indexer/events/mch/builders/*.py registrations): sync `(Block) -> bool`,
// consulted at MATCH time by the walker (pred-kind nodes, node-level named
// `where` clauses, pred anchors). The IR loader gates matcher runnability on
// this registry — a matcher whose pred names are all registered is runnable;
// the missing names become its skip reason. The Python twins mirror the
// registered-name list as a fixed surface contract (CPP_PREDS).
#pragma once

#include "ExprRuntime.h"
#include "Value.h"

#include <map>
#include <string>
#include <vector>

namespace mch {

struct Block;   // BlockTree.h
struct BlockArena;  // BlockTree.h
struct BuildEnv;  // BuildRuntime.h

using HostPredFn = bool (*)(const Block *);

// Registered predicate implementations, name -> fn (names are the Python
// registry keys).
const std::map<std::string, HostPredFn> &host_predicates();

// Host `fn` implementations (Python registries.fns): the build-time escape
// hatch. Unlike builtins, ARGUMENTS PASS THROUGH unchanged — a null arg is NOT
// short-circuited (the fn decides). Signature takes the BuildEnv so a fn can
// reach env.lookups (two-phase) / env.consumed. A thrown/faulted result is a
// clean match rejection (spec §10.7); returning Null likewise rejects when the
// spec `reject when`s the null.
using HostFn = EvalResult (*)(BuildEnv &, const std::vector<Value> &);

// Registered host fns, name -> fn (names are the Python registry keys). The IR
// loader / build_skip_reason gate a matcher's inclusion on this registry.
const std::map<std::string, HostFn> &host_fns();

// Host `shape` implementations (Python registries.shapers): a post-build
// topology pass (spec §8.7). Python signature is `shaper(produced, match)`;
// captured blocks are reached by name and an arena is needed to mint proxy
// blocks (Python EmptyBlock() self-allocates; C++ blocks are arena-owned), so
// both ride on ShaperMatch. No engine call site invokes shapers yet — no
// matcher has both a build_program and a `shape` (getgems has a `shape` but is
// builder-tier). Registered so build_skip_reason stops reporting a registered
// shaper as a missing binding, and so the --shape-test harness can drive it.
struct ShaperMatch {
  std::map<std::string, Block *> captures;  // capture name -> block (or absent)
  BlockArena *arena{nullptr};               // proxy-block allocation

  Block *capture(const std::string &name) const {
    auto it = captures.find(name);
    return it == captures.end() ? nullptr : it->second;
  }
};

using HostShaperFn = void (*)(Block *produced, const ShaperMatch &m);

// Registered host shapers, name -> fn (names are the Python registry keys).
const std::map<std::string, HostShaperFn> &host_shapers();

}  // namespace mch
