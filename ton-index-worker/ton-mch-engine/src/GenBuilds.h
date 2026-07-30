// Stable interface for the generated build-program functions (produced by
// `mch-codegen --builds <doc> --suffix <sfx>` as a CMake build step; the
// outputs live in the build tree, not in git). One function per matcher
// carrying a build_program. Each input document gets its OWN table symbols so
// all tables link into one binary and every consumer is permanently paired
// with its document:
//  - gen_builds_ir():      generated from ir/mch_ir.json (keyed by matcher
//    index in the artifact's `matchers` array) — classify / build-smoke;
//  - gen_builds_testbed(): OPTIONAL (-DMCH_TESTBED_IR=<artifact.json>) —
//    a phase's experimental artifact, so classify/build-smoke can run against
//    a testbed WITHOUT editing the ir/mch_ir.json contract mid-phase. Without
//    the cache var this resolves to the committed empty stub
//    (TestbedStubs.cpp; its sha "" never matches a real artifact). The knob
//    generates the matching where table too — see GenWheres.h.
// The *_source_sha() guards stay as a safety net: runners hard-fail when the
// sha differs from the document they load — with per-document generation the
// pairing is correct by construction, but a wrong pairing must remain
// impossible to run silently.
#pragma once

#include "BuildRuntime.h"

#include <string>
#include <vector>

namespace mch {

struct GenBuild {
  int id;            // matcher index (artifact) / vector index (build_vectors)
  const char *name;  // matcher / vector name (diagnostics)
  BuildOutcome (*fn)(BuildEnv &);
};

// Table from ir/mch_ir.json, ascending matcher index.
const std::vector<GenBuild> &gen_builds_ir();
const char *gen_builds_ir_source_sha();

// The conformance-vector table (ir/build_vectors.json) is test-only and is
// declared in fixtures/GenBuildsVectors.h, compiled into mch-fixtures.

// Table from ${MCH_TESTBED_IR} (or the empty stub when the var is unset).
// TODO: remove (harness): a testbed-artifact slot in the product lib
// (python-parity-audit §5.7 H4). It cannot move to mch-fixtures as written —
// select_gen_builds (BuildDriver.cpp) is called from ClassifyCore, so
// mch-classify must resolve gen_builds_testbed on its own; with the stub still
// lib-side a fixtures-side table would either duplicate-define it or lose to
// the stub in archive order, silently disabling the knob. Same argument now
// applies to gen_wheres_testbed (select_gen_wheres runs from ClassifyCore too).
const std::vector<GenBuild> &gen_builds_testbed();
const char *gen_builds_testbed_source_sha();

// The artifact-keyed table whose source sha matches `sha` (ir first, then
// testbed); nullptr when neither matches. `which` (optional) receives
// "ir"/"testbed" for diagnostics.
const std::vector<GenBuild> *select_gen_builds(const std::string &sha,
                                               const char **which = nullptr);

}  // namespace mch
