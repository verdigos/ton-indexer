// --classify-core-test: block-tree topology guards and the classify-failure
// fallback contract, over synthetic blocks and traces. Needs no fixtures.
#pragma once

namespace mch {

// Returns 0 when every check passes, 1 otherwise.
int run_classify_core_test();

}  // namespace mch
