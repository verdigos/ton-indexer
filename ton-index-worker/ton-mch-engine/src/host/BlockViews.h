// Narrow read-only views over blocks / consumed match sets for the host TUs
// (findings 2026-07-12 candidate 3). Only operations a host already demonstrates
// are here — this is NOT a graph-query language. Each op below has a 2nd use in
// the DEX cores (Stonfi v2 collects calls-by-op, lt-sorts swaps, finds a
// next-block call child) even though HostCoffee is the W1 pilot that consumes it.
#pragma once

#include <cstdint>
#include <vector>

namespace mch {

struct Block;

// Is `b` present in `v` (pointer identity)?
bool block_in(const std::vector<const Block *> &v, const Block *b);

// The first call_contract block in `blocks` whose opcode == `op`, or nullptr.
const Block *first_call(const std::vector<const Block *> &blocks, std::uint32_t op);

// Every call_contract block in `blocks` whose opcode == `op`, in input order.
std::vector<const Block *> all_calls(const std::vector<const Block *> &blocks, std::uint32_t op);

// The first next_block of `b` that is a call_contract with opcode == `op`.
const Block *first_next_call(const Block *b, std::uint32_t op);

// De-duplicate by pointer identity, then stable-sort by min_lt ascending. This
// is the exact HostCoffee swap-ordering (sort-by-ptr + unique + stable min_lt
// sort), reproduced so the peer-swap order is byte-identical.
void unique_lt_sorted(std::vector<const Block *> &blocks);

}  // namespace mch
