// Runs the generated expression-vector corpus and checks every result.
#pragma once

#include "Value.h"

#include "td/utils/JsonBuilder.h"

#include <string>

namespace mch {

int run_vectors_gen(const std::string &path);

// Tagged value encoding to Value ($amount/$account/$asset_*/$bytes_b64/
// $cell_b64/$obj/$list/$dict/plain). Shared with the build-vector runner.
Value decode_vector_value(const td::JsonValue &e);

}  // namespace mch
