// Stable interface for the generated build-program functions (produced by
// `mch-codegen --builds <doc> --suffix <sfx>` as a CMake build step; the outputs
// live in the build tree, not in git). One function per matcher carrying a
// build_program, keyed by its index in the document's `matchers` array.
//
// `gen_builds_ir()` is generated from MCH_CLASSIFY_ARTIFACT — ir/mch_ir.json, or
// the experimental artifact when -DMCH_TESTBED_IR=<artifact.json> is set. Which
// document feeds it is a COMPILE-time choice, so the matcher / build / where
// tables cannot come from different documents by accident, and no run-time table
// selection exists any more. prepare_classify still cross-checks the three
// tables' source shas, which is the only way that invariant can now break (a
// partial regeneration).
//
// The conformance-vector table (ir/build_vectors.json) is test-only and is
// declared in fixtures/GenBuildsVectors.h, compiled into mch-fixtures.
#pragma once

#include "BuildRuntime.h"

#include <vector>

namespace mch {

struct GenBuild {
  int id;            // matcher index (artifact) / vector index (build_vectors)
  const char *name;  // matcher / vector name (diagnostics)
  BuildOutcome (*fn)(BuildEnv &);
};

// Table from MCH_CLASSIFY_ARTIFACT, ascending matcher index.
const std::vector<GenBuild> &gen_builds_ir();

// SHA-256 (hex) of the document this table was generated from — the value
// prepare_classify compares against the matcher and where tables'.
const char *gen_builds_ir_source_sha();

}  // namespace mch
