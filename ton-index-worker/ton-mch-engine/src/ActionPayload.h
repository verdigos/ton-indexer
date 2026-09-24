#pragma once

#include "ActionBuild.h"

#include <string>

namespace mch {

std::string encode_action_payload(const Action &action);
std::string action_payload_bytes(const Action &action);

}  // namespace mch
