#pragma once

// AbiLeavesAddress: L1 address leaves (W4, T4.1). The four Tolk address kinds
// -- address / address? / ext_address / any_address -- each a load+store pair,
// fail-closed. This is the parity-trap wave: accepted inputs are pinned by
// vectors, but the REJECT space must be enumerated from the @ton/core source.
//
// NORMATIVE reference: @ton/core (vendored ton-abi/tools/node_modules/@ton/core):
//   BitReader.js  2-bit tag: 0=addr_none, 1=addr_extern, 2=addr_std, 3=addr_var.
//     loadAddress()          -> tag MUST be 2, else throw ("address" type)
//     loadMaybeAddress()     -> 0 -> null; 2 -> std; else throw ("address?")
//     loadExternalAddress()  -> tag MUST be 1, else throw
//     loadAddressAny()       -> 0->none, 2->std, 1->extern, 3->throw 'Unsupported'
//     addr_std body: tag(2) anycast(1) workchain(int8) hash(256) = 267 bits
//     addr_extern body: tag(2) len(uint9) value(len bits)
//   BitBuilder.js writeAddress: internal -> 10 + 0(anycast) + int8 wc + 256 hash;
//     external -> 01 + uint9 len + value; null -> 00.
//
// REJECT-space (one unit per condition in the test TU): addr_var tag (0b11),
// wrong tag for the specific kind, malformed/truncated extern length or value,
// truncated std body, AND -- see the discrepancy note in AbiLeavesAddress.cpp --
// the ANYCAST bit set (this leaf REJECTS it per the plan brief; @ton/core
// instead REWRITES the prefix. Flagged to the lead; reject is safe because no
// fixture/vector carries an anycast address and real messages never do).

#include "common/bitstring.h"
#include "common/refint.h"
#include "td/utils/Status.h"

#include "vm/cellslice.h"
#include "vm/cells/CellBuilder.h"

namespace ton_abi {

enum class AbiAddressKind { None, Std, Extern };

// One decoded address. §0.14 dump mapping (W8): Std -> "wc:hex"; Extern ->
// {"extern":{"bits":ext_bits,"value":<hex of ext_value>}}; None -> "none"
// (any_address) / null (address?). ext_value is a bit-slice (0 refs) because an
// external address value can be up to 511 bits -- wider than a RefInt256.
struct AbiAddress {
  AbiAddressKind kind = AbiAddressKind::None;

  // Std
  int workchain = 0;        // int8, signed
  td::Bits256 hash{};       // zero-initialized

  // Extern
  int ext_bits = 0;
  td::Ref<vm::CellSlice> ext_value;  // exactly ext_bits bits, 0 refs
};

// address: addr_std ONLY (tag 0b10); anycast set / any other tag -> error.
td::Result<AbiAddress> load_address(vm::CellSlice &cs);
td::Status store_address(vm::CellBuilder &cb, const AbiAddress &a);  // requires kind==Std

// address?: addr_none -> None; addr_std -> Std; anything else -> error.
td::Result<AbiAddress> load_maybe_address(vm::CellSlice &cs);
td::Status store_maybe_address(vm::CellBuilder &cb, const AbiAddress &a);  // None | Std

// ext_address: addr_extern ONLY (tag 0b01); any other tag -> error.
td::Result<AbiAddress> load_external_address(vm::CellSlice &cs);
td::Status store_external_address(vm::CellBuilder &cb, const AbiAddress &a);  // requires kind==Extern

// any_address: none / std / extern; addr_var (tag 0b11) -> error.
td::Result<AbiAddress> load_address_any(vm::CellSlice &cs);
td::Status store_address_any(vm::CellBuilder &cb, const AbiAddress &a);  // any kind

}  // namespace ton_abi
