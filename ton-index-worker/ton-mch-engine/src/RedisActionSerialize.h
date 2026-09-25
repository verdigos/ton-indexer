// Serializes mch::Action rows for the trace hash's `actions` field.
#pragma once

#include "ActionBuild.h"
#include "emu/EmuTypes.h"

#include <string>
#include <vector>

namespace mch {

// Returns an uncompressed msgpack array of action maps using Redis wire types.
// Each map stores DB payload bytes under binary `payload`; `view` supplies the
// trace key and minimum node finality.
std::string serialize_redis_actions(const std::vector<Action> &actions, const EmuTraceView &view);

}  // namespace mch
