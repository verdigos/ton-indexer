// --dexrecords-test: self-contained typed-host-seam unit tests (W1). Covers the
// DexRecords encoder shapes + null conventions, SwapRecord::fill_peer_out_assets,
// the HostAdapter arity/reject/fault policy, and the shared pTON wallet->asset
// helper. Same flag-driven pattern as --shape-test.
#pragma once

namespace mch {

// Returns 0 on all-pass, 1 on any failure.
int run_dexrecords_test();

}  // namespace mch
