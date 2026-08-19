#pragma once

#include <string>

namespace mch {

// Regenerates the cross-language fixture consumed by the Go MsgPack decoder
// round-trip test. Returns 0 on success.
int write_action_msgpack_fixture(const std::string &path);

}  // namespace mch
