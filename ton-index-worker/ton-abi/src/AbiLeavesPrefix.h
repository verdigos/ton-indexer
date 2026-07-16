#pragma once

// AbiLeavesPrefix: L1 prefix ops + union prefix-tree dispatch (W7, T7.1 -- the
// last leaf wave). Struct opcode checks, peek-based union dispatch, and the
// implicit-eats / explicit-re-reads / T|void / Either semantics -- all
// fail-closed. Inner value parse stays the walker's (W8) job.
//
// NORMATIVE reference: @ton/core-mirroring dynamic-serialization.ts:
//   lookupPrefix (:18-20): PEEK -- remainingBits >= len && preloadUint(len) == num.
//   union UNPACK (:376-411):
//     - trailing `void` is variants.back() (has_void). It does NOT participate
//       in prefix dispatch; it is matched by an EMPTY slice.
//     - EXACT T|void (2 variants, last void): empty slice -> void FIRST; else
//       take variant[0] (the T) unconditionally, skipping its implicit prefix
//       (no lookupPrefix) -- the discriminator is emptiness, not the prefix.
//     - general (>2 variants or no void): for each non-void variant in dispatch
//       order, lookupPrefix (peek); on match, if IMPLICIT skip(len) [EAT], if
//       EXPLICIT leave the bits (the variant's own loader RE-READS them); then
//       unpack. After the loop, if has_void && empty slice -> void, ELSE throw
//       ("none of union prefixes match") -- so void is matched LAST here, and a
//       non-empty no-match is an error (not silently void).
//   union PACK (:231-253): write the variant's prefix ONLY when IMPLICIT
//     (prefix_len > 0); for EXPLICIT the variant's own store writes it.
//   The static-codegen "Either bool fast path" (2-variant implicit,
//     emit-pack-unpack.ts:107-145) is a pure optimization: under loader-verified
//     prefix-freedom it is semantically identical to the general 1-bit dispatch,
//     so the interpreter path we mirror implements ONLY the general path.
//
// Prefix widths: union-variant prefixes are loader-bounded to <= 32 bits; STRUCT
// opcode prefixes go wider (48-bit exists) up to the cell limit -- so prefix_num
// is uint64 and len may be up to 1023 (W1 correction). For len > 64 the high
// (len-64) bits must be zero, since prefix_num fits in uint64.

#include "AbiKernel.h"  // ResolvedUnion / LabeledVariant (W2)

#include "td/utils/Status.h"

#include <cstdint>

#include "vm/cellslice.h"
#include "vm/cells/CellBuilder.h"

namespace ton_abi {

// Read `len` bits and require they equal `num` (the struct-opcode check). len==0
// always matches. Fail-closed on truncation or mismatch.
td::Status load_and_check_prefix(vm::CellSlice &cs, std::uint64_t num, int len);

// Write `len` bits of `num`.
td::Status store_prefix(vm::CellBuilder &cb, std::uint64_t num, int len);

// PEEK (no consume): true iff `len` bits are available and equal `num`.
bool lookup_prefix(const vm::CellSlice &cs, std::uint64_t num, int len);

// Result of union dispatch: which variant matched (index into
// ResolvedUnion::variants), and whether it is the trailing void (matched by an
// empty slice). For a matched non-void variant, an IMPLICIT prefix has already
// been consumed from `cs`; an EXPLICIT prefix is left in place for the variant's
// own loader to re-read.
struct UnionDispatch {
  std::size_t variant_index = 0;
  bool is_void = false;
};

// Dispatch a union against `cs`, consuming implicit prefixes per the reference
// semantics above. Fail-closed if nothing matches.
td::Result<UnionDispatch> dispatch_union(vm::CellSlice &cs, const ResolvedUnion &ru);

// Store side: write a variant's dispatch prefix -- ONLY when implicit (explicit
// prefixes are written by the variant type's own store).
td::Status store_union_prefix(vm::CellBuilder &cb, const LabeledVariant &v);

}  // namespace ton_abi
