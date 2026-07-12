// --msg-vectors <json>: run the M1 boundary-coverage message vectors (produced by
// msgs_gen.py alongside the generated parsers). Each case decodes a body BOC and
// asserts parse_message_body(type, body) succeeds ("ok") or fails ("fail").
#pragma once

#include <string>

namespace mch {

// Returns 0 when every case matches its expectation, 1 otherwise, 2 on load error.
int run_msg_vectors(const std::string &path);

}  // namespace mch
