// W12 mch <-> ton-abi bridge (design: W12-BRIDGE-DESIGN.md). Turns a Tolk-ABI
// generated struct (ton-abi-gen output) into an mch::Value so ABI-parsed message
// bodies plug into the existing message_parsers() registry alongside the hand and
// schema-generated parsers. Pure codegen (W11.5): no runtime ABI loading, no
// AbiModel, no init API -- registration is compile-time static rows.
//
// Dependency direction: mch -> ton-abi ONLY (ton-abi stays mch-free forever,
// plan decision 5). This header lives mch-side and includes ton-abi's AbiValue.
#pragma once

#include "Value.h"
#include "MsgParse.h"  // MsgParserFn

#include "AbiValue.h"  // ton-abi (linked into mch-classify)

#include "td/utils/Status.h"
#include "vm/cells/Cell.h"
#include "vm/cellslice.h"

#include <string>
#include <utility>
#include <vector>

namespace mch {

// AbiValue -> mch::Value adapter (impl in AbiBridge.cpp). TOLK-FAITHFUL value
// conventions (@ton/core / AbiValue.h dump semantics, NOT pytoniq). Total: never
// errors -- every AbiValueKind maps onto an existing VType (design §3). ABI bodies
// form a NEW value universe; this does NOT imitate PSlice's extern-string / None
// -> null conventions.
Value abi_value_to_mch(const ton_abi::AbiValue &v);

// Parse a message body as the generated ABI struct T, returning an mch::Value.
// T is a ton-abi-gen type (ton_abi::gen::<contract>::<Decl>) exposing:
//   static td::Result<T> from_slice(vm::CellSlice&);   // checks its own prefix
//   td::Result<ton_abi::AbiValue> to_abi_value() const;
//
// &abi_parse_body<T> has type td::Result<Value>(*)(const td::Ref<vm::Cell>&) ==
// MsgParserFn EXACTLY (no closure state -- dispatch is baked into from_slice), so
// the instantiation registers as an ordinary parser row. Top-level exotic cells
// are rejected here (W11.5 re-home) with a clean error result -- never an abort.
template <class T>
td::Result<Value> abi_parse_body(const td::Ref<vm::Cell> &body) {
  if (body.is_null()) {
    return td::Status::Error("abi bridge: null body cell");
  }
  bool special = false;
  vm::CellSlice cs = vm::load_cell_slice_special(body, special);
  if (special) {
    return td::Status::Error("abi bridge: exotic cell not supported");
  }
  TRY_RESULT(parsed, T::from_slice(cs));
  TRY_RESULT(av, parsed.to_abi_value());
  return abi_value_to_mch(av);
}

// The ABI-generated parser rows merged into message_parsers() (design §4).
// EMPTY for T12.1 -- the first production protocol row lands in T12.2. Rows are
// {"<contract_name>::<decl name>", &abi_parse_body<T>}.
const std::vector<std::pair<std::string, MsgParserFn>> &abi_message_parsers();

}  // namespace mch
