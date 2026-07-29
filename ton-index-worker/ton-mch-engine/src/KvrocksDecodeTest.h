// Decoder gate for the kvrocks tier-2 mapping (em2-tier2-design §10 W3).
// HARNESS-ONLY, hermetic: no store, no network.
//
// The vectors are NOT written down here — every payload under test is produced
// by `kvrocks_state::build_payload`, the SAME function the interface writer
// calls in production. Only the row VALUES are ours. That is the whole point:
// the risk this gate exists to catch is the decoder's reading of the wire format
// drifting from the writer's actual output, and a hand-copied payload would
// encode the very assumption under test. When the writer changes an encoding,
// this test fails instead of production silently mis-classifying.
#pragma once

namespace mch {

int run_kvrocks_decode_test();

}  // namespace mch
