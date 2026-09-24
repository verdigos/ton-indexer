// --actions-msgpack-test: self-contained tests for the emulator write-back
// envelope. It checks row columns, codec-prefixed binary payloads, finality,
// and fixture parity with the batch payload encoder.
#pragma once

#include <string>

namespace mch {

// Source of truth also consumed by the local dev-engine's
// --actions-msgpack-out fixture regeneration path.
std::string wire_fixture_bytes();

// Returns 0 on all-pass, 1 on any failure.
int run_action_msgpack_test();

}  // namespace mch
