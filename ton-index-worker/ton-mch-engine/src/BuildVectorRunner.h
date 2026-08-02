// Runs build-program conformance vectors against the generated build table.
// Each case supplies captures, block-backed values, lookups, and the expected
// BuildOutcome. The table's source hash must match the vector document.
#pragma once

#include <string>

namespace mch {

int run_build_vectors(const std::string &path);

}  // namespace mch
