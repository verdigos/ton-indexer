// Offline A/B gate for the production SchemaTraceLoader (Phase G step 3 slice 2,
// harness). `--classify-schema`: classify each fixture routed through
// msgpack->our-structs->schema->SchemaTraceLoader->our-structs (path B), so
// diffing it against `--classify` (path A) proves the schema loader reproduces
// the exact trees/actions.
#pragma once

#include <string>
#include <vector>

namespace mch {

int run_classify_schema(const std::string &artifact_path, const std::vector<std::string> &paths);

// Same gate, serialized-Action-row render (diff vs `--actions`). Account-case +
// aborted fidelity ride on this path too (raw account leaks into the accounts
// set; aborted flips success/failed).
int run_actions_schema(const std::string &artifact_path, const std::vector<std::string> &paths);

}  // namespace mch
