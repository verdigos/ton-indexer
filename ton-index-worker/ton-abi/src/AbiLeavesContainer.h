#pragma once

// AbiLeavesContainer: L1 arrayOf / lispListOf leaves (W6, T6.1). These are
// STRUCTURAL walkers over the container wire format; per-element recursion into
// the inner type is the caller's (W8 walker's) job, supplied as a callback --
// same one-semantics leaf for both the interpreter and generated code.
//
// NORMATIVE reference: @ton/core dynamic-serialization.ts (the interpreter our
// port mirrors):
//   arrayOf pack (:149-161): store uint8 length, then a maybeRef to a chain of
//     "chunk" cells built in REVERSE; each chunk = storeMaybeRef(nextChunk) then
//     one element. STORE = TS parity: 1 element per chunk cell (§0.12).
//   arrayOf unpack (:296-311): load uint8 length, load maybeRef head; for each
//     chunk cell FIRST loadMaybeRef (the next chunk), THEN drain ALL remaining
//     bits+refs as elements. This accepts BOTH the 1-elem/ref form AND the
//     compiler-chunked form (many elements per cell). Final count must equal the
//     length prefix (§0.12).
//   lispListOf pack (:162-172): fold from element 0; each node = pack(elem) then
//     storeRef(prevTail); outer stores a ref to the last node; terminator = the
//     empty cell (Cell.EMPTY).
//   lispListOf unpack (:312-323): load the head ref; while a node has a ref,
//     loadRef (the tail) FIRST, then unpack the element from the node's bits,
//     endParse (node must be exhausted), unshift; recurse into the tail. NB the
//     reference reads the tail as ref[0] and only works when the element type has
//     no refs of its own (an upstream limitation we inherit faithfully).

#include "td/utils/Status.h"

#include "vm/cellslice.h"
#include "vm/cells/CellBuilder.h"

#include <cstddef>
#include <functional>

namespace ton_abi {

// arrayOf. `unpack_one(elem_slice)` must consume exactly one element from the
// slice (advancing it). Returns error on count mismatch or malformed chain.
td::Status load_array(vm::CellSlice &cs,
                      const std::function<td::Status(vm::CellSlice &)> &unpack_one);

// `pack_one(builder, idx)` packs element `idx` into the given (fresh chunk)
// builder. STORE = TS parity (1 element per ref cell).
td::Status store_array(vm::CellBuilder &cb, std::size_t count,
                       const std::function<td::Status(vm::CellBuilder &, std::size_t)> &pack_one);

// lispListOf.
td::Status load_lisp_list(vm::CellSlice &cs,
                          const std::function<td::Status(vm::CellSlice &)> &unpack_one);
td::Status store_lisp_list(vm::CellBuilder &cb, std::size_t count,
                           const std::function<td::Status(vm::CellBuilder &, std::size_t)> &pack_one);

}  // namespace ton_abi
