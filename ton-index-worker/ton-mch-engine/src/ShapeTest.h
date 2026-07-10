// --shape-test mode: self-contained parity checks for the shaper substrate
// (Block::insert_between) and the registered getgems_proxy_insert shaper,
// mirroring the Python R06/R07 runtime tests (tests/mch_ir/test_shape.py).
// Prints one PASS/FAIL line per check; returns 0 iff all pass.
#pragma once

namespace mch {

int run_shape_test();

}  // namespace mch
