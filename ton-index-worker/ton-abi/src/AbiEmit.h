#pragma once

// AbiEmit: ton-abi-gen's code generator (W11, T11.1). Consumes a loaded
// ContractABI + its AbiKernel and produces a committed C++ source pair
// (generated/<contract_snake>_gen.{h,cpp}) of typed structs whose from_slice /
// store / to_abi_value bodies BAKE the kernel's resolution + dispatch tables
// -- a faithful C++ port of the reference emitter
// (S:\Dev\tolk-abi-to-typescript\src\generate-ts-wrappers.ts +
// emit-pack-unpack.ts), governed by ton-abi/W11-SHAPE-SPEC.md.
//
// Fail-closed: a decl-level non-standard map key (mapKV key not
// intN/uintN/address) is a HARD generation failure (shape spec §7 /
// ErrorsAtCodegen parity). Per-direction non-serializable fields
// (int/slice/builder/callable/unknown) become COMPILING STUBS that return a
// runtime error (§7 stub-vs-fail). No kernel/loader reference survives into the
// emitted code -- all dispatch is baked here (shape spec §1).

#include "AbiKernel.h"
#include "AbiModel.h"

#include "td/utils/Status.h"

#include <string>

namespace ton_abi {

struct GeneratedFiles {
  std::string contract_snake;  // sanitized snake name -> file/namespace base
  std::string header;          // <contract_snake>_gen.h contents
  std::string source;          // <contract_snake>_gen.cpp contents
};

// Emit the generated pair for `abi` (kernel resolves it). Fail-closed on
// non-standard map keys and any resolution error.
td::Result<GeneratedFiles> emit_abi(const ContractABI &abi, const AbiKernel &kernel);

}  // namespace ton_abi
