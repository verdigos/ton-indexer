// W12 mch <-> ton-abi bridge: AbiValue -> mch::Value adapter + the production
// ABI parser rows (3 live: the DeDust CPMM V2 claim_reward family, BARE decl
// names). See W12-BRIDGE-DESIGN.md §3/§4 + T122-CLAIMREWARD-DESIGN.md §3.
#include "AbiBridge.h"

#include "common/refint.h"
#include "vm/cells/CellBuilder.h"

// T12.2: first production ABI parser pair (DeDust CPMM V2 claim_reward family).
#include "dedust_v2_gen.h"

#include <string>

namespace mch {

using ton_abi::AbiValue;
using ton_abi::AbiValueKind;
using ton_abi::AbiAddressKind;

namespace {

// Build an mch::Value Cell from a bit-slice (bitsN / remaining / external-address
// value): copy the slice's bits+refs into a fresh cell. Lossless and root-hash
// comparable; avoids an {bits,refs} Obj shape and a new VType.
//
// The _bool overload is deliberate: `append_cellslice` THROWS vm::CellWriteError
// (ensure_pass) on overflow, and that type is neither vm::VmError nor a
// std::exception, so parse_message_body's soft-parse guards would NOT catch it --
// a throw here would break the adapter's totality contract (design §3/§5). The
// overflow itself is unreachable (a slice spans one cell: <=1023 bits / <=4 refs,
// exactly an empty builder's capacity); the empty-cell fallback just keeps
// totality a property of the code rather than of an argument about it.
Value cell_from_slice(const td::Ref<vm::CellSlice> &slice) {
  vm::CellBuilder cb;
  if (slice.not_null() && !cb.append_cellslice_bool(*slice)) {
    return Value::make_cell(vm::CellBuilder().finalize());
  }
  return Value::make_cell(cb.finalize());
}

Value adapt_address(const ton_abi::AbiAddress &a) {
  switch (a.kind) {
    case AbiAddressKind::Std: {
      // "wc:hex" -> canonical "wc:HEX" (uppercase raw form, AccountId.as_str parity).
      std::string raw = std::to_string(a.workchain) + ":" + a.hash.to_hex();
      auto canon = normalize_raw_address(raw);
      return Value::make_account_raw(canon ? *canon : raw);
    }
    case AbiAddressKind::None:
      // Present-but-empty address (addr_none from any_address). NOT null --
      // faithful to AccountId(None) vs a null Value.
      return Value::make_account_none();
    case AbiAddressKind::Extern: {
      // No native external-address VType (and no new VType, design §3). Mirror
      // AbiValue's {"extern":{bits,value}} as an Obj without the pytoniq string.
      Value::Fields f;
      f.emplace_back("bits", Value::make_int(td::make_refint(a.ext_bits)));
      f.emplace_back("value", cell_from_slice(a.ext_value));
      return Value::make_obj(std::move(f));
    }
  }
  return Value::null();  // unreachable
}

}  // namespace

Value abi_value_to_mch(const AbiValue &v) {
  switch (v.kind) {
    case AbiValueKind::Int:
      return Value::make_int(v.int_v);
    case AbiValueKind::Bool:
      return Value::make_bool(v.bool_v);
    case AbiValueKind::Address:
      return adapt_address(v.address_v);
    case AbiValueKind::Cell:
      return Value::make_cell(v.cell_v);
    case AbiValueKind::CellOf: {
      // Mirror AbiValue dump {"ref": inner}. `inner` is always set by the
      // emitter for this kind; the guard keeps the adapter total (no UB) if a
      // future producer ever hands over a moved-from value.
      Value::Fields f;
      f.emplace_back("ref", v.inner ? abi_value_to_mch(*v.inner) : Value::null());
      return Value::make_obj(std::move(f));
    }
    case AbiValueKind::Bits:
      return cell_from_slice(v.bits_v);
    case AbiValueKind::String:
      // Raw bytes into Str (byte-preserving). The lossy-UTF8 decode is a
      // dump-side concern (AbiValue::to_json), not re-imposed here.
      return Value::make_str(v.string_v);
    case AbiValueKind::List: {
      std::vector<Value> items;
      items.reserve(v.list_v.size());
      for (const auto &e : v.list_v) {
        items.push_back(abi_value_to_mch(e));
      }
      return Value::make_list(std::move(items));
    }
    case AbiValueKind::Struct: {
      // Obj (attribute access), "$" = struct name FIRST, then fields in decl
      // order -- mirrors the AbiValue dump so a consumer can switch on ".$".
      Value::Fields f;
      f.emplace_back("$", Value::make_str(v.struct_name));
      for (const auto &kv : v.struct_fields) {
        f.emplace_back(kv.first, abi_value_to_mch(kv.second));
      }
      return Value::make_obj(std::move(f));
    }
    case AbiValueKind::Union: {
      // has_value_field variants only reach here: {"$":label,"value":inner}.
      // Struct-labeled variants are Struct-kind (carry their own "$"). Same
      // totality guard on `inner` as the CellOf branch.
      Value::Fields f;
      f.emplace_back("$", Value::make_str(v.union_label));
      f.emplace_back("value", v.inner ? abi_value_to_mch(*v.inner) : Value::null());
      return Value::make_obj(std::move(f));
    }
    case AbiValueKind::Void: {
      Value::Fields f;
      f.emplace_back("$", Value::make_str("void"));
      return Value::make_obj(std::move(f));
    }
    case AbiValueKind::Null:
      return Value::null();
    case AbiValueKind::Map: {
      // Mirror dump [[k,v],...] in wire order; List-of-pairs preserves non-string
      // key types (mch Dict keys are std::string only).
      std::vector<Value> pairs;
      pairs.reserve(v.map_entries.size());
      for (const auto &kv : v.map_entries) {
        std::vector<Value> pair;
        pair.push_back(abi_value_to_mch(kv.first));
        pair.push_back(abi_value_to_mch(kv.second));
        pairs.push_back(Value::make_list(std::move(pair)));
      }
      return Value::make_list(std::move(pairs));
    }
  }
  return Value::null();  // unreachable
}

const std::vector<std::pair<std::string, MsgParserFn>> &abi_message_parsers() {
  // T12.2: first production rows. Keys are BARE decl names (not <contract>::<decl>)
  // -- the .mch grammar's `parse CAP as Name` reads a bare IDENT, all existing
  // registry rows are bare, and none of these 3 names collide (validate_registries
  // is the collision backstop). Acked §0.16 deviation; revisit on a real cross-ABI
  // name clash. See T122-CLAIMREWARD-DESIGN.md §3.
  namespace g = ton_abi::gen::dedust_v2;
  static const std::vector<std::pair<std::string, MsgParserFn>> rows = {
      {"ClaimRewardMessage", &abi_parse_body<g::ClaimRewardMessage>},
      {"PayoutRewardMessage", &abi_parse_body<g::PayoutRewardMessage>},
      {"PayoutMessage", &abi_parse_body<g::PayoutMessage>},
  };
  return rows;
}

}  // namespace mch
