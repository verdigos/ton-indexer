// Match-phase dump runner for the MCH IR walker. Blocks use canonical BFS order;
// matchers use priority order. Consumed keys are sorted, while list captures
// preserve assembly order and gap-aligned nulls.
#pragma once

#include <string>
#include <vector>

namespace mch {

// Dump matches over each path (files or directories of *.lz4) using the
// COMPILED-IN matcher table (GenMatchers.h) and the generated where table
// (GenWheres.h), the same match phase production classify runs.
int run_match(const std::vector<std::string> &paths);

}  // namespace mch
