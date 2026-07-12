// Accessor for the M1 schema-generated message-body parsers. The parser fns
// themselves are file-local in the generated TU (src/MsgParsersGenerated.cpp,
// produced by indexer/events/mch/msgs_gen.py from mch-msgs.schema); MsgParse.cpp
// merges these rows into message_parsers() alongside the kept-hand parsers.
#pragma once

#include "MsgParse.h"

#include <string>
#include <utility>
#include <vector>

namespace mch {

// name -> parser rows for every schema-generated message type, in schema order.
const std::vector<std::pair<std::string, MsgParserFn>> &generated_message_parsers();

}  // namespace mch
