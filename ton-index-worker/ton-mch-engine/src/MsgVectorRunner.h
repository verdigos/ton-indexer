// --msg-vectors <json>: run the frozen message boundary vectors. Each case
// asserts parse_message_body(type, body) succeeds/fails, and successful cases
// carrying `expect_value` must equal fixtures/Render.h's canonical parse render.
// --msg-vectors-regen <json> is the one-shot maintainer mode that atomically
// replaces `expect_value` on every successful case; fail cases remain status-only.
#pragma once

#include <string>

namespace mch {

// Returns 0 when every case matches its expectation, 1 otherwise, 2 on load error.
int run_msg_vectors(const std::string &path);

// Returns 0 after every case matched and the file was atomically rewritten.
int regen_msg_vectors(const std::string &path);

}  // namespace mch
