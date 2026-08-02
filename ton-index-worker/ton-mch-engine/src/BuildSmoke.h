// Runs the compiled matcher and build tables over fixtures without merging or
// reinserting produced blocks. This isolates the two-phase lookup and build path.
// Output contains sorted skip records and one rendered BuildOutcome per match.
#pragma once

#include <string>
#include <vector>

namespace mch {

int run_build_smoke(const std::vector<std::string> &paths);

}  // namespace mch
