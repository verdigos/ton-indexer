// W12 mch <-> ton-abi bridge: AbiValue -> mch::Value adapter + the (empty for
// T12.1) production ABI parser rows. See W12-BRIDGE-DESIGN.md §3/§4.
#include "AbiBridge.h"

#include "common/refint.h"
#include "vm/cells/CellBuilder.h"

#include <string>

namespace mch {

using ton_abi::AbiValue;
using ton_abi::AbiValueKind;
using ton_abi::AbiAddressKind;

namespace {

// Build an mch::Value Cell from a bit-slice (bitsN / remaining / external-address
// value): copy the slice's bits+refs into a fresh cell. Lossless and root-hash
// comparable; avoids an {bits,refs} Obj shape and a new VType.
Value cell_from_slice(const td::Ref<vm::CellSlice> &slice) {
  vm::CellBuilder cb;
  if (slice.not_null()) {
    cb.append_cellslice(*slice);
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
      // Mirror AbiValue dump {"ref": inner}.
      Value::Fields f;
      f.emplace_back("ref", abi_value_to_mch(*v.inner));
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
      // Struct-labeled variants are Struct-kind (carry their own "$").
      Value::Fields f;
      f.emplace_back("$", Value::make_str(v.union_label));
      f.emplace_back("value", abi_value_to_mch(*v.inner));
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
  // EMPTY for T12.1 (no production protocol row yet; T12.2 adds the first).
  static const std::vector<std::pair<std::string, MsgParserFn>> rows = {};
  return rows;
}

}  // namespace mch
