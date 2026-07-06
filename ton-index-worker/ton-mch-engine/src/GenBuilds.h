// Stable interface for the generated build-program functions
// (src/generated/builds_generated.cpp, produced by `mch-codegen --builds`).
// One function per matcher carrying a build_program. The generator accepts
// either an IR artifact (keyed by matcher index in the artifact's `matchers`
// array) or a build_vectors.json (keyed by vector index) — both are stable
// ids for their input document. A table generated from a different document
// than the one being run is a hard error at the runner level.
#pragma once

#include "BuildRuntime.h"

#include <vector>

namespace mch {

struct GenBuild {
  int id;            // matcher index (artifact) / vector index (build_vectors)
  const char *name;  // matcher / vector name (diagnostics)
  BuildOutcome (*fn)(BuildEnv &);
};

// The generated table, ascending id.
const std::vector<GenBuild> &gen_builds();

// SHA-256 (hex) of the source document the table was generated from. Runners
// hard-fail when it differs from the document they load — a wrong table/
// artifact pairing must be impossible to run silently.
const char *gen_builds_source_sha();

}  // namespace mch
