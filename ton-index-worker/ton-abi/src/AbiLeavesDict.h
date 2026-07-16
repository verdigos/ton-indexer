#pragma once

// AbiLeavesDict: L1 mapKV leaf (W6, T6.1). A thin adapter over vm::Dictionary
// for the HashmapE wire form (§0.14 map). Keys are fixed-width (intN/uintN =>
// n bits, address => 267 bits -- the reference's own restriction, @ton/core
// createTonCoreDictionaryKey / dynamic-serialization.ts). Per-entry KEY and
// VALUE parsing stays the WALKER's job (W8): the leaf yields key-slice +
// value-slice pairs in ascending raw-key-bit order.
//
// ORDER: load yields entries in vm::Dictionary check_for_each ascending order,
// which is the HashmapE's raw-key-bit order -- byte-for-byte the same order
// @ton/core's Dictionary serializes/iterates in. For unsigned/address keys this
// is numeric order; for signed intN keys negatives sort last (two's-complement
// high bit). Both sides agree, so no side needs to re-sort for §0.14's
// "sorted by key" (see the W6 report note).
//
// STORE: builds vm::Dictionary and append_dict_to_bool -- PROVEN byte-identical
// to @ton/core storeDict/serializeDict in AbiLeavesDictTest (a dict built both
// sides serializes to the same BOC), so no custom label writer is needed
// (policy §3 W6 row).

#include "td/utils/Status.h"

#include "vm/cellslice.h"
#include "vm/cells/CellBuilder.h"

#include <cstddef>
#include <functional>

namespace ton_abi {

// mapKV load. Parses the HashmapE from `cs` (advancing past the maybe-bit +
// optional ref), then calls `on_entry(key_slice, value_slice)` per entry in
// ascending key order. An absent dict (maybe bit 0) yields no calls.
td::Status load_dict(vm::CellSlice &cs, int key_bits,
                     const std::function<td::Status(vm::CellSlice &, vm::CellSlice &)> &on_entry);

// mapKV store. `emit(idx, key_out, val_out)` must write EXACTLY key_bits into
// key_out and the value body into val_out for entry idx. Serializes as HashmapE
// (maybe bit + ref). Duplicate keys are rejected.
td::Status store_dict(vm::CellBuilder &cb, int key_bits, std::size_t count,
                      const std::function<td::Status(std::size_t, vm::CellBuilder &, vm::CellBuilder &)> &emit);

}  // namespace ton_abi
