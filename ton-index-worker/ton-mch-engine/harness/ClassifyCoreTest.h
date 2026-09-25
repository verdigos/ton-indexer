// Classify-core gate: topology, fallback and schema outcomes, using synthetic
// traces and the simple TON-transfer fixture.
#pragma once

namespace mch {

// Returns 0 when every check passes, 1 otherwise.
int run_classify_core_test();

}  // namespace mch
