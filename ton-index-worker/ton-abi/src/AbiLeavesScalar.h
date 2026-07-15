#pragma once

// AbiLeavesScalar: L1 scalar leaves (W3, T3.1). Each Tolk scalar type gets a
// load+store pair over vm::CellSlice / vm::CellBuilder, fail-closed (td::Result
// / td::Status, never abort). Values are td::RefInt256 for the integer kinds
// and bool for bool; bitsN is a bit-string, returned as a Ref<vm::CellSlice> of
// exactly n bits / 0 refs (its §0.14 slice-ish dump is W8's job -- the leaf
// only moves bits).
//
// NORMATIVE reference: @ton/core (vendored at ton-abi/tools/node_modules/
// @ton/core), whose semantics the interpreter must reproduce exactly:
//   BitReader.js   loadIntBig / loadUintBig (two's-complement, sign bit first)
//                  loadVarUintBig(bits): size = loadUint(bits) BYTES, then
//                  loadUintBig(size*8); loadVarIntBig likewise signed;
//                  loadCoins() = loadVarUintBig(4)
//   BitBuilder.js  writeUint (v in [0,2^bits)), writeInt (bits==1 -> {-1,0};
//                  else [-2^(bits-1), 2^(bits-1))), writeVarUint (v>=0; v==0 ->
//                  zero size; else sizeBytes = ceil(bitlen(v)/8)),
//                  writeVarInt (sizeBytes = ceil((bitlen(|v|)+1)/8), +1 for
//                  the sign bit -> NB differs from a two's-complement bit_size
//                  for exact powers of two like -128), writeCoins = writeVarUint(_,4)
// The var* size-prefix WIDTH is log2(n) bits, per dynamic-serialization.ts
// (loadVarIntBig(Math.log2(ty.n)) / storeVarInt(v, Math.log2(ty.n))) -- verified
// against BitReader/BitBuilder. n MUST be a power of two (var_prefix_bits fails
// closed otherwise).

#include "common/refint.h"
#include "td/utils/Status.h"

#include "vm/cellslice.h"
#include "vm/cells/CellBuilder.h"

namespace ton_abi {

// Size-prefix bit width for varintN/varuintN: log2(n). Fails closed if n is not
// a positive power of two.
td::Result<int> var_prefix_bits(int n);

// intN (n in [1,257]): two's-complement signed, n bits.
td::Result<td::RefInt256> load_int(vm::CellSlice &cs, int n);
td::Status store_int(vm::CellBuilder &cb, const td::RefInt256 &v, int n);

// uintN (n in [1,256]): unsigned, n bits.
td::Result<td::RefInt256> load_uint(vm::CellSlice &cs, int n);
td::Status store_uint(vm::CellBuilder &cb, const td::RefInt256 &v, int n);

// bool: a single bit.
td::Result<bool> load_bool(vm::CellSlice &cs);
td::Status store_bool(vm::CellBuilder &cb, bool v);

// coins: varuint with a 4-bit size prefix (@ton/core loadCoins/storeCoins).
td::Result<td::RefInt256> load_coins(vm::CellSlice &cs);
td::Status store_coins(vm::CellBuilder &cb, const td::RefInt256 &v);

// varintN / varuintN: log2(n)-bit byte-count prefix, then that many bytes,
// signed / unsigned respectively.
td::Result<td::RefInt256> load_varint(vm::CellSlice &cs, int n);
td::Status store_varint(vm::CellBuilder &cb, const td::RefInt256 &v, int n);
td::Result<td::RefInt256> load_varuint(vm::CellSlice &cs, int n);
td::Status store_varuint(vm::CellBuilder &cb, const td::RefInt256 &v, int n);

// bitsN (n in [0,1023]): exactly n bits, 0 refs.
td::Result<td::Ref<vm::CellSlice>> load_bits(vm::CellSlice &cs, int n);
td::Status store_bits(vm::CellBuilder &cb, const vm::CellSlice &v, int n);

}  // namespace ton_abi
