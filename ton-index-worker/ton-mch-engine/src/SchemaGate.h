// Offline A/B gate for the production SchemaTraceLoader (Phase G step 3 slice 2,
// harness). `--classify-schema`: classify each fixture routed through
// msgpack->our-structs->schema->SchemaTraceLoader->our-structs (path B), so
// diffing it against `--classify` (path A) proves the schema loader reproduces
// the exact trees/actions.
#pragma once

#include <string>
#include <vector>

namespace mch {

int run_classify_schema(const std::vector<std::string> &paths);

// FULL production-path gate (slice 3): schema loader trace + ParsedBlockLookupSource
// (synthesized account_interfaces_ tier-1 + fixture-backed tier-2 fallback),
// diffed against `--classify` / `--actions`. Proves the production lookup adapter
// reproduces the fixture source's shapes end-to-end. Expected diff 0.
int run_classify_prod(const std::vector<std::string> &paths);
int run_actions_prod(const std::vector<std::string> &paths);

// Same gate, serialized-Action-row render (diff vs `--actions`). Account-case +
// aborted fidelity ride on this path too (raw account leaks into the accounts
// set; aborted flips success/failed).
int run_actions_schema(const std::vector<std::string> &paths);

}  // namespace mch
