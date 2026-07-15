#pragma once

// AbiModel: ContractABI/Ty PODs (~30 kinds; mirrors abi.ts+abi-types.ts).
// W0 scaffold only — the real model lands in W1 (T1.1). Kept as a
// non-empty placeholder TU so the ton-abi static lib has a real
// compilation unit from the start.

namespace ton_abi {

// Scaffold marker, removed once W1 adds real declarations.
int scaffold_marker();

}  // namespace ton_abi
