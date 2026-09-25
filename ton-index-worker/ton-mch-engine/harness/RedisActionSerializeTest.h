// --actions-msgpack-test: self-contained tests for the emulator write-back
// envelope, compact payloads, and finality.
#pragma once

namespace mch {

// Returns 0 on all-pass, 1 on any failure.
int run_action_msgpack_test();

}  // namespace mch
