// --actions-msgpack-test: self-contained unit tests for the write-back writer.
// Covers the presence rule (absent vs null), the natural-types rule (which
// fields are decimal strings and which naturalize), the []string element
// discipline, the Value->msgpack table, the bucket-B fills, the synthetic
// `unknown` row, and the two insert-side decisions (view_finality and the
// actions staleness guard). Same flag-driven pattern as --shape-test /
// --dexrecords-test.
#pragma once

#include <string>

namespace mch {

// Returns 0 on all-pass, 1 on any failure.
int run_action_msgpack_test();

// --actions-msgpack-out: regenerates the cross-language wire fixture the Go
// round-trip test consumes (ton-index-go/index/emulated/testdata/). The bytes
// are the ones test_wire_fixture asserts on, so the two halves of the contract
// cannot drift. Returns 0 on success.
int write_action_msgpack_fixture(const std::string &path);

}  // namespace mch
