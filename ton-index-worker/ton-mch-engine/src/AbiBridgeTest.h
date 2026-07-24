// --abi-bridge-test mode: self-contained checks for the W12 ton-abi bridge
// (AbiBridge.{h,cpp}). Drives the trampoline (from_slice -> to_abi_value ->
// adapter) over committed generated fixture structs, plus direct AbiValue->Value
// adapter units for the kinds no fixture reaches, plus the registry dup policy.
// Prints one PASS/FAIL line per check; returns 0 iff all pass. See
// W12-BRIDGE-DESIGN.md §7.
#pragma once

namespace mch {

int run_abi_bridge_test();

}  // namespace mch
