// --actions-msgpack-test: self-contained unit tests for the EM2 write-back
// writer (em2-writeback-design.md §2). Covers the presence rule (absent vs
// null), the top-level int/str split, address/amount stringification, the §2.6
// integer divergence, the Value->msgpack table and the empty-actions payload.
// Same flag-driven pattern as --shape-test / --dexrecords-test.
#pragma once

namespace mch {

// Returns 0 on all-pass, 1 on any failure.
int run_action_msgpack_test();

}  // namespace mch
