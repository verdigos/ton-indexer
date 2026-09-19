// Test-only build-program conformance-vector table.
// Generated from ir/build_vectors.json
// (`mch-codegen --builds --suffix vectors`). Product tables are in GenBuilds.h.
#pragma once

#include "GenBuilds.h"  // GenBuild

#include <vector>

namespace mch {

// Ascending vector index.
const std::vector<GenBuild> &gen_builds_vectors();
const char *gen_builds_vectors_source_sha();

}  // namespace mch
