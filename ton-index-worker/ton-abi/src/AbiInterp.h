#pragma once

// AbiInterp: the interpreter walkers (W8, T8.2). A faithful C++ port of
// S:\Dev\tolk-abi-to-typescript\src\dynamic-serialization.ts's dynamicPack /
// dynamicUnpack switch, driving the W3-W7 leaves and the W2 kernel's resolved
// model. AbiValue (T8.1) is the value currency in both directions.
//
// AbiInterpCtx mirrors dynamic-ctx.ts's DynamicCtx: one kernel + one LOCAL
// custom-serializer registry (§0.17's "(a) INTERPRETER registry"), keyed by
// plain declaration name (struct_name / alias_name / enum_name) -- exactly
// like the reference's registerCustomPackUnpack(typeName, ...). The bridge
// (W12) is what layers a <contract>::<decl> GLOBAL key on top of this; that's
// out of scope here.
//
// CUSTOM-GATE ORDERING (W2 invariant ii, non-negotiable): for StructRef /
// AliasRef / EnumRef, the custom_pack_unpack check on the DECL happens BEFORE
// struct_fields_of / alias_target_of is ever called -- exactly
// dynamic-serialization.ts:335-374's order. Per the reference (and the
// upstream "custom serializers not on generics" limitation, plan §B / §5),
// only the GENERIC declaration's custom_pack_unpack is consulted; a struct/
// alias INSTANTIATION's own custom_pack_unpack field (ABIStructInstantiation /
// ABIAliasInstantiation) is not wired to anything here -- getStruct(name) in
// the reference always resolves the generic decl by name, never the
// per-instantiation record.
//
// uLabelTyIdx THREADING (W2 invariant i): struct fields and alias targets
// carry it forward into unions; NULLABLE DROPS IT (dynamic-serialization.ts:
// 291) -- the recursive call for a nullable's inner type never receives one,
// even if the nullable field itself had one.
//
// Field-path convention (mirrors the reference exactly, for error-message and
// debugging parity): struct field -> "path.name"; arrayOf/lispListOf element
// -> "path[ith]" (the reference passes the LITERAL string "[ith]", not the
// real index); tensor/shapedTuple element -> "path[idx]" (real index here);
// union variant -> "path#label".

#include "AbiKernel.h"
#include "AbiValue.h"

#include "td/utils/Status.h"

#include "vm/cells/Cell.h"
#include "vm/cells/CellSlice.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace ton_abi {

using CustomPackFn = std::function<td::Status(const AbiValue &, vm::CellBuilder &)>;
using CustomUnpackFn = std::function<td::Result<AbiValue>(vm::CellSlice &)>;

class AbiInterpCtx {
 public:
  explicit AbiInterpCtx(const AbiKernel &kernel) : kernel_(&kernel) {}

  const AbiKernel &kernel() const {
    return *kernel_;
  }

  // Registers pack and/or unpack for decl `name` (struct/alias/enum). Either
  // fn may be an empty std::function -- pack-only / unpack-only registration
  // is legal (OnlyWithPack / OnlyWithUnpack, §0.17). Re-registering the same
  // name is an error (mirrors DynamicCtx.registerCustomPackUnpack).
  td::Status register_custom(const std::string &name, CustomPackFn pack_fn, CustomUnpackFn unpack_fn);

  // nullptr if nothing (or no pack/unpack half) was registered for `name`.
  const CustomPackFn *find_custom_pack(const std::string &name) const;
  const CustomUnpackFn *find_custom_unpack(const std::string &name) const;

 private:
  const AbiKernel *kernel_;
  std::unordered_map<std::string, std::pair<CustomPackFn, CustomUnpackFn>> registry_;
};

// §0.17 (a): the interpreter-side custom-serializer registration entry point.
// Thin wrapper over AbiInterpCtx::register_custom, named per the plan/handoff.
td::Status register_abi_custom_dynamic(AbiInterpCtx &ctx, const std::string &name, CustomPackFn pack_fn,
                                        CustomUnpackFn unpack_fn);

// Recursive unpack/pack walkers. `field_path` is for error-message locality
// only (see the convention note above); `u_label_ty_idx` threads the union
// label back-reference (W2 invariant i).
td::Result<AbiValue> unpack_value(const AbiInterpCtx &ctx, int ty_idx, vm::CellSlice &cs, const std::string &field_path,
                                   std::optional<int> u_label_ty_idx = std::nullopt);

td::Status pack_value(const AbiInterpCtx &ctx, int ty_idx, const AbiValue &v, vm::CellBuilder &cb,
                       const std::string &field_path, std::optional<int> u_label_ty_idx = std::nullopt);

// Top-level entry (mirrors unpackFromSliceDynamic, but over a whole message
// body cell rather than an already-open slice): rejects exotic/special root
// cells (§R.6) fail-closed, then unpacks `ty_idx` from it.
td::Result<AbiValue> unpack_message_body(const AbiInterpCtx &ctx, int ty_idx, td::Ref<vm::Cell> body);

}  // namespace ton_abi
