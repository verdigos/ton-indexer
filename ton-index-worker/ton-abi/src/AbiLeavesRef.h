#pragma once

// AbiLeavesRef: L1 ref / string / remaining / maybe leaves (W5, T5.1). Each is
// a thin move over vm::CellSlice / vm::CellBuilder; recursion into an inner type
// is the WALKER's job (W8) -- these leaves only shuttle refs, bits, and bytes.
//
// NORMATIVE reference: @ton/core (vendored ton-abi/tools/node_modules/@ton/core):
//   Slice.js/Builder.js loadRef/storeRef, loadMaybeRef/storeMaybeRef (1-bit
//     presence + optional ref).
//   dynamic-serialization.ts cellOf = storeRef(pack(inner)) / loadRef().beginParse()
//     then unpack(inner); nullable = 1-bit maybe then inner; remaining = snapshot
//     the rest (bits + refs) via clone/storeSlice.
//   strings.js snake format (readBuffer/writeBuffer): each cell holds byte-ALIGNED
//     bytes (remainingBits % 8 == 0, else throw "Invalid string length") and at
//     most ONE continuation ref (remainingRefs in {0,1}); writer chunks at
//     floor(1023/8)=127 bytes per cell. storeStringRefTail(s) = storeRef(snake
//     cell of s); loadStringRefTail = readString(loadRef().beginParse()). The
//     leaf moves raw bytes; the lossy-UTF8 decode for the JSON dump is W8's job.
//
// BOC serialization mode (W0-deferred §0.14 item, pinned here -- see
// VALUE_SCHEMA.md): @ton/core cell.toBoc() defaults to { idx:false, crc32:true },
// which equals vm::std_boc_serialize(root, 2) (Mode::WithCRC32C). Byte-identity
// proven by AbiLeavesRefTest against a Node-generated reference BOC.

#include "td/utils/Status.h"

#include "vm/cellslice.h"
#include "vm/cells/CellBuilder.h"

#include <string>

namespace ton_abi {

// cell: an opaque whole cell, stored/loaded as a ref (no inner parse).
td::Result<td::Ref<vm::Cell>> load_cell(vm::CellSlice &cs);
td::Status store_cell(vm::CellBuilder &cb, td::Ref<vm::Cell> cell);  // also used for cellOf store

// cellOf: fetch the ref and open it as a slice for the walker to parse the
// inner type. Rejects a special/exotic ref (fail-closed). Store side = store_cell.
td::Result<td::Ref<vm::CellSlice>> load_ref_slice(vm::CellSlice &cs);

// maybe_ref: 1-bit presence, then an optional ref. Absent -> null Ref.
td::Result<td::Ref<vm::Cell>> load_maybe_ref(vm::CellSlice &cs);
td::Status store_maybe_ref(vm::CellBuilder &cb, td::Ref<vm::Cell> maybe_cell);

// nullable presence prefix: the single 1-bit maybe flag (inner recursion is the
// walker's). true = value present.
td::Result<bool> load_maybe_prefix(vm::CellSlice &cs);
td::Status store_maybe_prefix(vm::CellBuilder &cb, bool present);

// string: snake ref-tail. Returns the raw bytes (UTF-8 decode is W8's dump job).
td::Result<std::string> load_string(vm::CellSlice &cs);
td::Status store_string(vm::CellBuilder &cb, const std::string &s);

// remaining (RemainingBitsAndRefs): snapshot the rest of the slice -- ALL
// remaining bits and refs -- draining the source. Store appends bits + refs.
td::Result<td::Ref<vm::CellSlice>> load_remaining(vm::CellSlice &cs);
td::Status store_remaining(vm::CellBuilder &cb, const vm::CellSlice &v);

}  // namespace ton_abi
