// Serializes mch::Action rows for the trace hash's `actions` field.
#pragma once

#include "ActionBuild.h"
#include "emu/EmuTypes.h"

#include <cstddef>
#include <string>
#include <vector>

namespace mch {

// Counts unexpected row-column Value shapes serialized with their defined fallback encoding.
struct RedisActionSerializeStats {
  std::size_t unrenderable{0};  // Unexpected shape or out-of-int64 integer encoded as nil.
};

// Returns an uncompressed msgpack array of action maps. Each map keeps the row
// columns in their native wire types and stores the DB payload bytes under the
// binary `payload` key. An empty action list is a one-byte array; an empty
// string means no actions field. The view supplies the trace key and minimum
// node finality.
std::string serialize_redis_actions(const std::vector<Action> &actions, const EmuTraceView &view,
                                   RedisActionSerializeStats *stats = nullptr);

}  // namespace mch
