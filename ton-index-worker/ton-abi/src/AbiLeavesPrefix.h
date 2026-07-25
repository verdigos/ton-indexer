#pragma once

// AbiLeavesPrefix: L1 prefix ops (W7, T7.1 -- the last leaf wave). Struct opcode
// check/write plus the peek primitive that generated union dispatch chains are
// built from -- all fail-closed.
//
// The union DISPATCH RULES themselves live in the emitter (AbiEmit.cpp, union
// chain emission), which bakes them per union from the generation-time
// ResolvedUnion; there is no runtime union-dispatch helper (W11.5: pure
// codegen). Those baked rules, from @ton/core-mirroring
// dynamic-serialization.ts union UNPACK (:376-411) / PACK (:231-253), are:
//   - trailing `void` does NOT participate in prefix dispatch; it is matched by
//     an EMPTY slice;
//   - exact T|void (2 variants, last void): empty -> void FIRST, else take the T
//     unconditionally (the discriminator is emptiness, not the prefix);
//   - general form: probe non-void variants in dispatch order with lookupPrefix
//     (peek); IMPLICIT match eats its prefix bits, EXPLICIT leaves them for the
//     variant's own loader to RE-READ; void matched LAST, and a non-empty
//     no-match is an error, not a silent void;
//   - pack writes a variant's prefix only when IMPLICIT.
// Generated-path coverage: test/AbiABGateTest.cpp ("union dispatch forms").
//
// lookup_prefix mirrors lookupPrefix (dynamic-serialization.ts:18-20): PEEK --
// remainingBits >= len && preloadUint(len) == num.
//
// Prefix widths: union-variant prefixes are loader-bounded to <= 32 bits; STRUCT
// opcode prefixes go wider (48-bit exists) up to the cell limit -- so prefix_num
// is uint64 and len may be up to 1023 (W1 correction). For len > 64 the high
// (len-64) bits must be zero, since prefix_num fits in uint64.

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

}  // namespace ton_abi
