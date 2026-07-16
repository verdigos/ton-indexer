#include "AbiInterp.h"

#include "AbiLeavesAddress.h"
#include "AbiLeavesContainer.h"
#include "AbiLeavesDict.h"
#include "AbiLeavesPrefix.h"
#include "AbiLeavesRef.h"
#include "AbiLeavesScalar.h"

#include "vm/boc.h"

namespace ton_abi {

// ---------------------------------------------------------------------------
// AbiInterpCtx / registry (§0.17 (a), mirrors dynamic-ctx.ts DynamicCtx)
// ---------------------------------------------------------------------------

td::Status AbiInterpCtx::register_custom(const std::string &name, CustomPackFn pack_fn, CustomUnpackFn unpack_fn) {
  if (registry_.count(name)) {
    return td::Status::Error(PSLICE() << "custom pack/unpack for '" << name << "' already registered");
  }
  registry_[name] = {std::move(pack_fn), std::move(unpack_fn)};
  return td::Status::OK();
}

const CustomPackFn *AbiInterpCtx::find_custom_pack(const std::string &name) const {
  auto it = registry_.find(name);
  if (it == registry_.end() || !it->second.first) {
    return nullptr;
  }
  return &it->second.first;
}

const CustomUnpackFn *AbiInterpCtx::find_custom_unpack(const std::string &name) const {
  auto it = registry_.find(name);
  if (it == registry_.end() || !it->second.second) {
    return nullptr;
  }
  return &it->second.second;
}

td::Status register_abi_custom_dynamic(AbiInterpCtx &ctx, const std::string &name, CustomPackFn pack_fn,
                                        CustomUnpackFn unpack_fn) {
  return ctx.register_custom(name, std::move(pack_fn), std::move(unpack_fn));
}

namespace {

// ---- field-path helpers (mirror the reference's exact conventions) --------

std::string field_of(const std::string &path, const std::string &name) {
  return path + "." + name;
}
std::string ith_of(const std::string &path) {
  return path + "[ith]";  // the reference passes the LITERAL string "[ith]", not an index
}
std::string idx_of(const std::string &path, std::size_t idx) {
  return path + "[" + std::to_string(idx) + "]";
}
std::string variant_of(const std::string &path, const std::string &label) {
  return path + "#" + label;
}

std::string kind_name(AbiValueKind k) {
  switch (k) {
    case AbiValueKind::Int: return "Int";
    case AbiValueKind::Bool: return "Bool";
    case AbiValueKind::Address: return "Address";
    case AbiValueKind::Cell: return "Cell";
    case AbiValueKind::CellOf: return "CellOf";
    case AbiValueKind::Bits: return "Bits";
    case AbiValueKind::String: return "String";
    case AbiValueKind::List: return "List";
    case AbiValueKind::Struct: return "Struct";
    case AbiValueKind::Union: return "Union";
    case AbiValueKind::Void: return "Void";
    case AbiValueKind::Null: return "Null";
    case AbiValueKind::Map: return "Map";
  }
  return "?";
}

td::Status wrong_kind(const std::string &field_path, const char *expected, const AbiValue &v) {
  return td::Status::Error(PSLICE() << field_path << ": expected " << expected << " value, got " << kind_name(v.kind));
}

const AbiValue *find_field(const AbiValue &v, const std::string &name) {
  for (const auto &f : v.struct_fields) {
    if (f.first == name) {
      return &f.second;
    }
  }
  return nullptr;
}

// mapKV key type -> wire bit width. Only intN/uintN/address are standard
// (createTonCoreDictionaryKey, dynamic-serialization.ts:22-29); anything else
// is a non-standard key @ton/core can't handle -- fail closed.
td::Result<int> dict_key_bits(const AbiKernel &kernel, int key_ty_idx, const std::string &field_path) {
  TRY_RESULT(kty, kernel.ty_by_idx(key_ty_idx));
  if (kty->kind == TyKind::IntN || kty->kind == TyKind::UintN) {
    return std::get<TyWidth>(kty->data).n;
  }
  if (kty->kind == TyKind::Address) {
    return 267;  // tag(2) + anycast(1) + workchain(8) + hash(256), same shape as addr_std
  }
  return td::Status::Error(PSLICE() << field_path << ": non-standard map key type (only intN/uintN/address supported)");
}

td::Result<vm::CellSlice> open_root_reject_special(const td::Ref<vm::Cell> &cell) {
  bool is_special = false;
  vm::CellSlice cs = vm::load_cell_slice_special(cell, is_special);
  if (is_special) {
    return td::Status::Error("unpack_message_body: root cell is exotic/special (rejected)");
  }
  return cs;
}

}  // namespace

// ---------------------------------------------------------------------------
// unpack
// ---------------------------------------------------------------------------

td::Result<AbiValue> unpack_value(const AbiInterpCtx &ctx, int ty_idx, vm::CellSlice &cs, const std::string &field_path,
                                   std::optional<int> u_label_ty_idx) {
  const AbiKernel &kernel = ctx.kernel();
  TRY_RESULT(ty, kernel.ty_by_idx(ty_idx));

  switch (ty->kind) {
    case TyKind::IntN: {
      TRY_RESULT_PREFIX(v, load_int(cs, std::get<TyWidth>(ty->data).n), PSLICE() << field_path << ": ");
      return AbiValue::make_int(std::move(v));
    }
    case TyKind::UintN: {
      TRY_RESULT_PREFIX(v, load_uint(cs, std::get<TyWidth>(ty->data).n), PSLICE() << field_path << ": ");
      return AbiValue::make_int(std::move(v));
    }
    case TyKind::VarIntN: {
      TRY_RESULT_PREFIX(v, load_varint(cs, std::get<TyWidth>(ty->data).n), PSLICE() << field_path << ": ");
      return AbiValue::make_int(std::move(v));
    }
    case TyKind::VarUIntN: {
      TRY_RESULT_PREFIX(v, load_varuint(cs, std::get<TyWidth>(ty->data).n), PSLICE() << field_path << ": ");
      return AbiValue::make_int(std::move(v));
    }
    case TyKind::Coins: {
      TRY_RESULT_PREFIX(v, load_coins(cs), PSLICE() << field_path << ": ");
      return AbiValue::make_int(std::move(v));
    }
    case TyKind::Bool: {
      TRY_RESULT_PREFIX(v, load_bool(cs), PSLICE() << field_path << ": ");
      return AbiValue::make_bool(v);
    }
    case TyKind::Cell: {
      TRY_RESULT_PREFIX(v, load_cell(cs), PSLICE() << field_path << ": ");
      return AbiValue::make_cell(std::move(v));
    }
    case TyKind::String: {
      TRY_RESULT_PREFIX(v, load_string(cs), PSLICE() << field_path << ": ");
      return AbiValue::make_string(std::move(v));
    }
    case TyKind::Remaining: {
      TRY_RESULT_PREFIX(v, load_remaining(cs), PSLICE() << field_path << ": ");
      return AbiValue::make_bits(std::move(v));
    }
    case TyKind::Address: {
      TRY_RESULT_PREFIX(v, load_address(cs), PSLICE() << field_path << ": ");
      return AbiValue::make_address(std::move(v));
    }
    case TyKind::AddressOpt: {
      TRY_RESULT_PREFIX(v, load_maybe_address(cs), PSLICE() << field_path << ": ");
      if (v.kind == AbiAddressKind::None) {
        return AbiValue::make_null();  // address? absent -> JSON null (NOT "none" -- that's any_address only)
      }
      return AbiValue::make_address(std::move(v));
    }
    case TyKind::AddressExt: {
      TRY_RESULT_PREFIX(v, load_external_address(cs), PSLICE() << field_path << ": ");
      return AbiValue::make_address(std::move(v));
    }
    case TyKind::AddressAny: {
      TRY_RESULT_PREFIX(v, load_address_any(cs), PSLICE() << field_path << ": ");
      return AbiValue::make_address(std::move(v));  // None dumps "none" here (see AbiValue::dump_address)
    }
    case TyKind::BitsN: {
      TRY_RESULT_PREFIX(v, load_bits(cs, std::get<TyWidth>(ty->data).n), PSLICE() << field_path << ": ");
      return AbiValue::make_bits(std::move(v));
    }
    case TyKind::NullLiteral:
      return AbiValue::make_null();
    case TyKind::Void:
      return AbiValue::make_void();

    case TyKind::Nullable: {
      TRY_RESULT_PREFIX(present, load_maybe_prefix(cs), PSLICE() << field_path << ": ");
      if (!present) {
        return AbiValue::make_null();
      }
      // uLabelTyIdx DROPPED here (W2 invariant, dynamic-serialization.ts:291) --
      // the inner recursion never inherits this field's own uLabel.
      return unpack_value(ctx, std::get<TyNullable>(ty->data).inner_ty_idx, cs, field_path);
    }

    case TyKind::CellOf: {
      TRY_RESULT_PREFIX(inner_cs_ref, load_ref_slice(cs), PSLICE() << field_path << ": ");
      vm::CellSlice inner_cs = *inner_cs_ref;  // Ref<CellSlice> only exposes const access; copy for a mutable cursor
      TRY_RESULT(inner_v, unpack_value(ctx, std::get<TyInner>(ty->data).inner_ty_idx, inner_cs, field_path));
      return AbiValue::make_cell_of(std::move(inner_v));
    }

    case TyKind::ArrayOf: {
      int inner_ty_idx = std::get<TyInner>(ty->data).inner_ty_idx;
      std::vector<AbiValue> items;
      td::Status st = load_array(cs, [&](vm::CellSlice &elem) -> td::Status {
        TRY_RESULT(v, unpack_value(ctx, inner_ty_idx, elem, ith_of(field_path)));
        items.push_back(std::move(v));
        return td::Status::OK();
      });
      TRY_STATUS_PREFIX(std::move(st), PSLICE() << field_path << ": ");
      return AbiValue::make_list(std::move(items));
    }

    case TyKind::LispListOf: {
      int inner_ty_idx = std::get<TyInner>(ty->data).inner_ty_idx;
      std::vector<AbiValue> items;
      td::Status st = load_lisp_list(cs, [&](vm::CellSlice &elem) -> td::Status {
        TRY_RESULT(v, unpack_value(ctx, inner_ty_idx, elem, ith_of(field_path)));
        items.push_back(std::move(v));
        return td::Status::OK();
      });
      TRY_STATUS_PREFIX(std::move(st), PSLICE() << field_path << ": ");
      return AbiValue::make_list(std::move(items));
    }

    case TyKind::Tensor:
    case TyKind::ShapedTuple: {
      const auto &items_ty_idx = std::get<TyItems>(ty->data).items_ty_idx;
      std::vector<AbiValue> items;
      items.reserve(items_ty_idx.size());
      for (std::size_t i = 0; i < items_ty_idx.size(); ++i) {
        TRY_RESULT(v, unpack_value(ctx, items_ty_idx[i], cs, idx_of(field_path, i)));
        items.push_back(std::move(v));
      }
      return AbiValue::make_list(std::move(items));
    }

    case TyKind::MapKV: {
      const auto &m = std::get<TyMapKV>(ty->data);
      TRY_RESULT(kty, kernel.ty_by_idx(m.key_ty_idx));
      TRY_RESULT(key_bits, dict_key_bits(kernel, m.key_ty_idx, field_path));
      std::vector<std::pair<AbiValue, AbiValue>> entries;
      td::Status st = load_dict(cs, key_bits, [&](vm::CellSlice &key_cs, vm::CellSlice &val_cs) -> td::Status {
        AbiValue key;
        if (kty->kind == TyKind::IntN) {
          TRY_RESULT(kv, load_int(key_cs, std::get<TyWidth>(kty->data).n));
          key = AbiValue::make_int(std::move(kv));
        } else if (kty->kind == TyKind::UintN) {
          TRY_RESULT(kv, load_uint(key_cs, std::get<TyWidth>(kty->data).n));
          key = AbiValue::make_int(std::move(kv));
        } else {
          TRY_RESULT(kv, load_address(key_cs));
          key = AbiValue::make_address(std::move(kv));
        }
        TRY_RESULT(val, unpack_value(ctx, m.value_ty_idx, val_cs, field_path));
        entries.emplace_back(std::move(key), std::move(val));
        return td::Status::OK();
      });
      TRY_STATUS_PREFIX(std::move(st), PSLICE() << field_path << ": ");
      return AbiValue::make_map(std::move(entries));
    }

    case TyKind::EnumRef: {
      const auto &name = std::get<TyEnumRef>(ty->data).enum_name;
      TRY_RESULT(decl, kernel.get_enum(name));
      if (decl->custom_pack_unpack && decl->custom_pack_unpack->unpack_from_slice) {
        const CustomUnpackFn *fn = ctx.find_custom_unpack(name);
        if (!fn) {
          return td::Status::Error(PSLICE() << field_path << ": custom unpackFromSlice was not registered for '"
                                             << name << "'");
        }
        return (*fn)(cs);
      }
      return unpack_value(ctx, decl->encoded_as_ty_idx, cs, field_path);
    }

    case TyKind::StructRef: {
      const auto &name = std::get<TyStructRef>(ty->data).struct_name;
      TRY_RESULT(decl, kernel.get_struct(name));
      if (decl->custom_pack_unpack && decl->custom_pack_unpack->unpack_from_slice) {
        const CustomUnpackFn *fn = ctx.find_custom_unpack(name);
        if (!fn) {
          return td::Status::Error(PSLICE() << field_path << ": custom unpackFromSlice was not registered for '"
                                             << name << "'");
        }
        return (*fn)(cs);
      }
      if (decl->prefix) {
        TRY_STATUS_PREFIX(load_and_check_prefix(cs, decl->prefix->prefix_num, decl->prefix->prefix_len),
                           PSLICE() << field_path << ": ");
      }
      TRY_RESULT(fields, kernel.struct_fields_of(ty_idx));
      AbiValue result = AbiValue::make_struct(name);
      for (const auto &f : *fields) {
        TRY_RESULT(fv, unpack_value(ctx, f.ty_idx, cs, field_of(field_path, f.name()), f.u_label_ty_idx));
        result.add_field(f.name(), std::move(fv));
      }
      return result;
    }

    case TyKind::AliasRef: {
      const auto &name = std::get<TyAliasRef>(ty->data).alias_name;
      TRY_RESULT(decl, kernel.get_alias(name));
      if (decl->custom_pack_unpack && decl->custom_pack_unpack->unpack_from_slice) {
        const CustomUnpackFn *fn = ctx.find_custom_unpack(name);
        if (!fn) {
          return td::Status::Error(PSLICE() << field_path << ": custom unpackFromSlice was not registered for '"
                                             << name << "'");
        }
        return (*fn)(cs);
      }
      TRY_RESULT(target, kernel.alias_target_of(ty_idx));
      return unpack_value(ctx, target.ty_idx, cs, field_path, target.u_label_ty_idx);
    }

    case TyKind::Union: {
      TRY_RESULT(ru, kernel.resolve_union(ty_idx, u_label_ty_idx));
      TRY_RESULT_PREFIX(d, dispatch_union(cs, ru), PSLICE() << field_path << ": ");
      if (d.is_void) {
        return AbiValue::make_void();
      }
      const LabeledVariant &variant = ru.variants[d.variant_index];
      TRY_RESULT(v, unpack_value(ctx, variant.variant_ty_idx, cs, variant_of(field_path, variant.label_str)));
      if (variant.has_value_field) {
        return AbiValue::make_union(variant.label_str, std::move(v));
      }
      return v;  // struct-with-its-own-label: no wrapper, dump is identical to v's own dump
    }

    // Non-serializable (get-method / stack domain, out of scope per plan
    // decision 1):
    case TyKind::Int:
      return td::Status::Error(PSLICE() << field_path << ": type 'int' is not serializable");
    case TyKind::Builder:
      return td::Status::Error(PSLICE() << field_path << ": type 'builder' is not serializable");
    case TyKind::Slice:
      return td::Status::Error(PSLICE() << field_path << ": type 'slice' is not serializable");
    case TyKind::Callable:
      return td::Status::Error(PSLICE() << field_path << ": type 'continuation' is not serializable");
    case TyKind::Unknown:
      return td::Status::Error(PSLICE() << field_path << ": type 'unknown' is not serializable");
    case TyKind::GenericT:
      return td::Status::Error(PSLICE() << field_path << ": unexpected genericT");
  }
  return td::Status::Error(PSLICE() << field_path << ": unhandled ty kind");
}

// ---------------------------------------------------------------------------
// pack
// ---------------------------------------------------------------------------

td::Status pack_value(const AbiInterpCtx &ctx, int ty_idx, const AbiValue &v, vm::CellBuilder &cb,
                       const std::string &field_path, std::optional<int> u_label_ty_idx) {
  const AbiKernel &kernel = ctx.kernel();
  TRY_RESULT(ty, kernel.ty_by_idx(ty_idx));

  switch (ty->kind) {
    case TyKind::IntN: {
      if (v.kind != AbiValueKind::Int) return wrong_kind(field_path, "Int", v);
      TRY_STATUS_PREFIX(store_int(cb, v.int_v, std::get<TyWidth>(ty->data).n), PSLICE() << field_path << ": ");
      return td::Status::OK();
    }
    case TyKind::UintN: {
      if (v.kind != AbiValueKind::Int) return wrong_kind(field_path, "Int", v);
      TRY_STATUS_PREFIX(store_uint(cb, v.int_v, std::get<TyWidth>(ty->data).n), PSLICE() << field_path << ": ");
      return td::Status::OK();
    }
    case TyKind::VarIntN: {
      if (v.kind != AbiValueKind::Int) return wrong_kind(field_path, "Int", v);
      TRY_STATUS_PREFIX(store_varint(cb, v.int_v, std::get<TyWidth>(ty->data).n), PSLICE() << field_path << ": ");
      return td::Status::OK();
    }
    case TyKind::VarUIntN: {
      if (v.kind != AbiValueKind::Int) return wrong_kind(field_path, "Int", v);
      TRY_STATUS_PREFIX(store_varuint(cb, v.int_v, std::get<TyWidth>(ty->data).n), PSLICE() << field_path << ": ");
      return td::Status::OK();
    }
    case TyKind::Coins: {
      if (v.kind != AbiValueKind::Int) return wrong_kind(field_path, "Int", v);
      TRY_STATUS_PREFIX(store_coins(cb, v.int_v), PSLICE() << field_path << ": ");
      return td::Status::OK();
    }
    case TyKind::Bool: {
      if (v.kind != AbiValueKind::Bool) return wrong_kind(field_path, "Bool", v);
      TRY_STATUS_PREFIX(store_bool(cb, v.bool_v), PSLICE() << field_path << ": ");
      return td::Status::OK();
    }
    case TyKind::Cell: {
      if (v.kind != AbiValueKind::Cell) return wrong_kind(field_path, "Cell", v);
      TRY_STATUS_PREFIX(store_cell(cb, v.cell_v), PSLICE() << field_path << ": ");
      return td::Status::OK();
    }
    case TyKind::String: {
      if (v.kind != AbiValueKind::String) return wrong_kind(field_path, "String", v);
      TRY_STATUS_PREFIX(store_string(cb, v.string_v), PSLICE() << field_path << ": ");
      return td::Status::OK();
    }
    case TyKind::Remaining: {
      if (v.kind != AbiValueKind::Bits) return wrong_kind(field_path, "Bits", v);
      TRY_STATUS_PREFIX(store_remaining(cb, *v.bits_v), PSLICE() << field_path << ": ");
      return td::Status::OK();
    }
    case TyKind::Address: {
      if (v.kind != AbiValueKind::Address) return wrong_kind(field_path, "Address", v);
      TRY_STATUS_PREFIX(store_address(cb, v.address_v), PSLICE() << field_path << ": ");
      return td::Status::OK();
    }
    case TyKind::AddressOpt: {
      if (v.kind == AbiValueKind::Null) {
        AbiAddress none;  // kind defaults to None
        TRY_STATUS_PREFIX(store_maybe_address(cb, none), PSLICE() << field_path << ": ");
        return td::Status::OK();
      }
      if (v.kind != AbiValueKind::Address) return wrong_kind(field_path, "Address or Null", v);
      TRY_STATUS_PREFIX(store_maybe_address(cb, v.address_v), PSLICE() << field_path << ": ");
      return td::Status::OK();
    }
    case TyKind::AddressExt: {
      if (v.kind != AbiValueKind::Address) return wrong_kind(field_path, "Address", v);
      TRY_STATUS_PREFIX(store_external_address(cb, v.address_v), PSLICE() << field_path << ": ");
      return td::Status::OK();
    }
    case TyKind::AddressAny: {
      if (v.kind != AbiValueKind::Address) return wrong_kind(field_path, "Address", v);
      TRY_STATUS_PREFIX(store_address_any(cb, v.address_v), PSLICE() << field_path << ": ");
      return td::Status::OK();
    }
    case TyKind::BitsN: {
      if (v.kind != AbiValueKind::Bits) return wrong_kind(field_path, "Bits", v);
      int n = std::get<TyWidth>(ty->data).n;
      if (static_cast<int>(v.bits_v->size()) != n || v.bits_v->size_refs() != 0) {
        return td::Status::Error(PSLICE() << field_path << ": expected " << n << " bits and 0 refs, got "
                                           << v.bits_v->size() << " bits and " << v.bits_v->size_refs() << " refs");
      }
      TRY_STATUS_PREFIX(store_bits(cb, *v.bits_v, n), PSLICE() << field_path << ": ");
      return td::Status::OK();
    }
    case TyKind::NullLiteral:
      return td::Status::OK();  // no wire representation (reference's pack switch has no case for it either)
    case TyKind::Void:
      return td::Status::OK();

    case TyKind::Nullable: {
      if (v.kind == AbiValueKind::Null) {
        TRY_STATUS_PREFIX(store_maybe_prefix(cb, false), PSLICE() << field_path << ": ");
        return td::Status::OK();
      }
      TRY_STATUS_PREFIX(store_maybe_prefix(cb, true), PSLICE() << field_path << ": ");
      // uLabelTyIdx dropped here too, symmetric with unpack.
      return pack_value(ctx, std::get<TyNullable>(ty->data).inner_ty_idx, v, cb, field_path);
    }

    case TyKind::CellOf: {
      if (v.kind != AbiValueKind::CellOf) return wrong_kind(field_path, "CellOf", v);
      vm::CellBuilder inner_cb;
      TRY_STATUS(pack_value(ctx, std::get<TyInner>(ty->data).inner_ty_idx, *v.inner, inner_cb, field_of(field_path, "ref")));
      TRY_STATUS_PREFIX(store_cell(cb, inner_cb.finalize()), PSLICE() << field_path << ": ");
      return td::Status::OK();
    }

    case TyKind::ArrayOf: {
      if (v.kind != AbiValueKind::List) return wrong_kind(field_path, "List", v);
      int inner_ty_idx = std::get<TyInner>(ty->data).inner_ty_idx;
      td::Status st = store_array(cb, v.list_v.size(), [&](vm::CellBuilder &elem_cb, std::size_t idx) -> td::Status {
        return pack_value(ctx, inner_ty_idx, v.list_v[idx], elem_cb, ith_of(field_path));
      });
      TRY_STATUS_PREFIX(std::move(st), PSLICE() << field_path << ": ");
      return td::Status::OK();
    }

    case TyKind::LispListOf: {
      if (v.kind != AbiValueKind::List) return wrong_kind(field_path, "List", v);
      int inner_ty_idx = std::get<TyInner>(ty->data).inner_ty_idx;
      td::Status st = store_lisp_list(cb, v.list_v.size(), [&](vm::CellBuilder &elem_cb, std::size_t idx) -> td::Status {
        return pack_value(ctx, inner_ty_idx, v.list_v[idx], elem_cb, ith_of(field_path));
      });
      TRY_STATUS_PREFIX(std::move(st), PSLICE() << field_path << ": ");
      return td::Status::OK();
    }

    case TyKind::Tensor:
    case TyKind::ShapedTuple: {
      if (v.kind != AbiValueKind::List) return wrong_kind(field_path, "List", v);
      const auto &items_ty_idx = std::get<TyItems>(ty->data).items_ty_idx;
      if (v.list_v.size() != items_ty_idx.size()) {
        return td::Status::Error(PSLICE() << field_path << ": expected " << items_ty_idx.size() << " items, got "
                                           << v.list_v.size());
      }
      for (std::size_t i = 0; i < items_ty_idx.size(); ++i) {
        TRY_STATUS(pack_value(ctx, items_ty_idx[i], v.list_v[i], cb, idx_of(field_path, i)));
      }
      return td::Status::OK();
    }

    case TyKind::MapKV: {
      if (v.kind != AbiValueKind::Map) return wrong_kind(field_path, "Map", v);
      const auto &m = std::get<TyMapKV>(ty->data);
      TRY_RESULT(kty, kernel.ty_by_idx(m.key_ty_idx));
      TRY_RESULT(key_bits, dict_key_bits(kernel, m.key_ty_idx, field_path));
      td::Status st = store_dict(
          cb, key_bits, v.map_entries.size(),
          [&](std::size_t idx, vm::CellBuilder &key_out, vm::CellBuilder &val_out) -> td::Status {
            const auto &entry = v.map_entries[idx];
            if (kty->kind == TyKind::IntN) {
              TRY_STATUS(store_int(key_out, entry.first.int_v, std::get<TyWidth>(kty->data).n));
            } else if (kty->kind == TyKind::UintN) {
              TRY_STATUS(store_uint(key_out, entry.first.int_v, std::get<TyWidth>(kty->data).n));
            } else {
              TRY_STATUS(store_address(key_out, entry.first.address_v));
            }
            return pack_value(ctx, m.value_ty_idx, entry.second, val_out, field_path);
          });
      TRY_STATUS_PREFIX(std::move(st), PSLICE() << field_path << ": ");
      return td::Status::OK();
    }

    case TyKind::EnumRef: {
      const auto &name = std::get<TyEnumRef>(ty->data).enum_name;
      TRY_RESULT(decl, kernel.get_enum(name));
      if (decl->custom_pack_unpack && decl->custom_pack_unpack->pack_to_builder) {
        const CustomPackFn *fn = ctx.find_custom_pack(name);
        if (!fn) {
          return td::Status::Error(PSLICE() << field_path << ": custom packToBuilder was not registered for '"
                                             << name << "'");
        }
        return (*fn)(v, cb);
      }
      return pack_value(ctx, decl->encoded_as_ty_idx, v, cb, field_path);
    }

    case TyKind::StructRef: {
      const auto &name = std::get<TyStructRef>(ty->data).struct_name;
      TRY_RESULT(decl, kernel.get_struct(name));
      if (decl->custom_pack_unpack && decl->custom_pack_unpack->pack_to_builder) {
        const CustomPackFn *fn = ctx.find_custom_pack(name);
        if (!fn) {
          return td::Status::Error(PSLICE() << field_path << ": custom packToBuilder was not registered for '"
                                             << name << "'");
        }
        return (*fn)(v, cb);
      }
      if (v.kind != AbiValueKind::Struct) return wrong_kind(field_path, "Struct", v);
      if (decl->prefix) {
        TRY_STATUS_PREFIX(store_prefix(cb, decl->prefix->prefix_num, decl->prefix->prefix_len),
                           PSLICE() << field_path << ": ");
      }
      TRY_RESULT(fields, kernel.struct_fields_of(ty_idx));
      for (const auto &f : *fields) {
        const AbiValue *fv = find_field(v, f.name());
        if (!fv) {
          return td::Status::Error(PSLICE() << field_of(field_path, f.name()) << ": missing field '" << f.name()
                                             << "'");
        }
        TRY_STATUS(pack_value(ctx, f.ty_idx, *fv, cb, field_of(field_path, f.name()), f.u_label_ty_idx));
      }
      return td::Status::OK();
    }

    case TyKind::AliasRef: {
      const auto &name = std::get<TyAliasRef>(ty->data).alias_name;
      TRY_RESULT(decl, kernel.get_alias(name));
      if (decl->custom_pack_unpack && decl->custom_pack_unpack->pack_to_builder) {
        const CustomPackFn *fn = ctx.find_custom_pack(name);
        if (!fn) {
          return td::Status::Error(PSLICE() << field_path << ": custom packToBuilder was not registered for '"
                                             << name << "'");
        }
        return (*fn)(v, cb);
      }
      TRY_RESULT(target, kernel.alias_target_of(ty_idx));
      return pack_value(ctx, target.ty_idx, v, cb, field_path, target.u_label_ty_idx);
    }

    case TyKind::Union: {
      TRY_RESULT(ru, kernel.resolve_union(ty_idx, u_label_ty_idx));

      // hasNull shortcut (dynamic-serialization.ts:234-236): a bare Null value
      // packs directly to a nullLiteral variant's prefix, UNCONDITIONALLY
      // (not gated on is_prefix_implicit -- this is the reference's own
      // asymmetry: the shortcut writes the full prefix itself since there's no
      // recursive pack call for a nullLiteral variant to do it instead).
      if (v.kind == AbiValueKind::Null) {
        for (const auto &variant : ru.variants) {
          TRY_RESULT(vty, kernel.ty_by_idx(variant.variant_ty_idx));
          if (vty->kind == TyKind::NullLiteral) {
            TRY_STATUS_PREFIX(store_prefix(cb, variant.prefix_num, variant.prefix_len), PSLICE() << field_path << ": ");
            return td::Status::OK();
          }
        }
        return td::Status::Error(PSLICE() << field_path << ": union has no nullLiteral variant to pack a null value into");
      }

      // Trailing void variant (has_void): a Void-kind value packs to it --
      // writes its prefix ONLY if implicit (store_union_prefix already
      // encodes that gate); typically prefix_len==0 for T|void, so nothing at
      // all is written, mirroring "empty slice = void" on unpack.
      if (v.kind == AbiValueKind::Void) {
        if (!ru.has_void) {
          return td::Status::Error(PSLICE() << field_path << ": union has no void variant to pack");
        }
        const LabeledVariant &variant = ru.variants.back();
        TRY_STATUS_PREFIX(store_union_prefix(cb, variant), PSLICE() << field_path << ": ");
        return td::Status::OK();
      }

      // General path: match by the value's own "$" identity, exactly the
      // reference's `v.$ === variant.labelStr` (dynamic-serialization.ts:239).
      // Union-kind v (has_value_field==true variants) carries the label
      // explicitly; a bare Struct-kind v (has_value_field==false, "struct
      // carries its own label" case, T8.1's simplification) carries it as
      // its own struct_name -- see the accepted design note from T8.1.
      std::string label;
      bool want_has_value_field;
      const AbiValue *value_to_pack = &v;
      if (v.kind == AbiValueKind::Union) {
        label = v.union_label;
        want_has_value_field = true;
        value_to_pack = v.inner.get();
      } else if (v.kind == AbiValueKind::Struct) {
        label = v.struct_name;
        want_has_value_field = false;
        value_to_pack = &v;  // the struct packs itself (variant_ty_idx's own StructRef reads named fields)
      } else {
        return td::Status::Error(PSLICE() << field_path
                                           << ": expected a union value ({$,value} wrapper) or a labeled struct, got "
                                           << kind_name(v.kind));
      }

      const LabeledVariant *found = nullptr;
      for (const auto &variant : ru.variants) {
        if (variant.has_value_field == want_has_value_field && variant.label_str == label) {
          found = &variant;
          break;
        }
      }
      if (!found) {
        return td::Status::Error(PSLICE() << field_path << ": non-existing union variant for $ = '" << label << "'");
      }
      TRY_STATUS_PREFIX(store_union_prefix(cb, *found), PSLICE() << field_path << ": ");
      return pack_value(ctx, found->variant_ty_idx, *value_to_pack, cb, variant_of(field_path, label));
    }

    case TyKind::Int:
      return td::Status::Error(PSLICE() << field_path << ": type 'int' is not serializable");
    case TyKind::Builder:
      return td::Status::Error(PSLICE() << field_path << ": type 'builder' is not serializable");
    case TyKind::Slice:
      return td::Status::Error(PSLICE() << field_path << ": type 'slice' is not serializable");
    case TyKind::Callable:
      return td::Status::Error(PSLICE() << field_path << ": type 'continuation' is not serializable");
    case TyKind::Unknown:
      return td::Status::Error(PSLICE() << field_path << ": type 'unknown' is not serializable");
    case TyKind::GenericT:
      return td::Status::Error(PSLICE() << field_path << ": unexpected genericT");
  }
  return td::Status::Error(PSLICE() << field_path << ": unhandled ty kind");
}

// ---------------------------------------------------------------------------
// top-level entry
// ---------------------------------------------------------------------------

td::Result<AbiValue> unpack_message_body(const AbiInterpCtx &ctx, int ty_idx, td::Ref<vm::Cell> body) {
  TRY_RESULT(cs, open_root_reject_special(body));
  return unpack_value(ctx, ty_idx, cs, "self");
}

}  // namespace ton_abi
