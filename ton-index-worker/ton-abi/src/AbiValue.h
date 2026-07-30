#pragma once

// AbiValue: a standalone, protocol-agnostic tagged value tree. Re-scoped at
// W11.5 (interpreter removed): it is now (a) the medium every generated
// struct's to_abi_value() dumps into -- the A/B gate compares that dump against
// the TS oracle vectors -- and (b) the interchange type the mch bridge adapts
// into mch::Value.
// Owns the canonical JSON dump (docs/ton-abi-plan.md §0.14 / MODULE-NOTES.md
// point 5-9,12), which is the format the conformance vectors compare against.
//
// (b) put it ON the parse hot path as of W12: every ABI-path message parse
// materializes the body THREE times -- typed struct (from_slice) -> AbiValue
// (to_abi_value) -> mch::Value (abi_value_to_mch, AbiBridge.h). Known cost,
// accepted for now: it keeps ton-abi mch-free and the vector gate exact.
//
// Kind -> payload -> JSON dump:
//   Int      RefInt256              decimal STRING (also used for `enum`)
//   Bool     bool                   JSON bool
//   Address  AbiAddress             Std "wc:hex" | Extern {"extern":{...}} |
//                                   None "none" (any_address only -- addressOpt
//                                   absent is Null, not this kind, see below)
//   Cell     Ref<Cell>              base64(std_boc_serialize(root,2))
//   CellOf   inner (unique_ptr)     {"ref": <inner>}
//   Bits     Ref<CellSlice>         {"bits":"0101..","refs":[boc-b64,...]}
//                                   (bitsN / remaining)
//   String   std::string            JSON string (raw bytes, walker decodes)
//   List     vector<AbiValue>       JSON array (arrayOf/lispListOf/tensor/
//                                   shapedTuple)
//   Struct   name + ordered fields  {"$":name, field: <v>, ...} in DECL order
//   Union    label + inner          {"$":label,"value":<inner>} -- ONLY used
//                                   when has_value_field is true; when false
//                                   the walker uses `inner` directly (it's
//                                   already a Struct carrying its own "$")
//   Void     (none)                 {"$":"void"}
//   Null     (none)                 JSON null (nullLiteral AND addressOpt-
//                                   absent AND nullable-absent all dump here)
//   Map      vector<pair<K,V>>      [[<key>,<value>], ...] in WIRE (ascending
//                                   raw-key-bit) order -- that IS §0.14's
//                                   "sorted by key" per the W6 dict-order note;
//                                   no re-sort at dump time.
//
// Recursive members needing a single embedded AbiValue (CellOf ref, Union
// wrapped value) use unique_ptr<AbiValue> indirection -- same reasoning as
// AbiModel.h's ABIConstExpression (a class can't contain itself by value).
// vector<AbiValue> / vector<pair<..,AbiValue>> members do NOT need this: a
// standard container only needs its element type complete when its own
// members are instantiated, which happens after this class is fully defined.

#include "AbiLeavesAddress.h"  // AbiAddress

#include "common/refint.h"

#include "vm/cells/Cell.h"
#include "vm/cells/CellSlice.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ton_abi {

enum class AbiValueKind {
  Int, Bool, Address, Cell, CellOf, Bits, String, List, Struct, Union, Void, Null, Map,
};

struct AbiValue {
  AbiValueKind kind = AbiValueKind::Null;

  td::RefInt256 int_v;                            // Int
  bool bool_v = false;                             // Bool
  AbiAddress address_v;                            // Address
  td::Ref<vm::Cell> cell_v;                        // Cell
  std::unique_ptr<AbiValue> inner;                 // CellOf ref / Union wrapped value
  td::Ref<vm::CellSlice> bits_v;                   // Bits (bitsN / remaining)
  std::string string_v;                            // String
  std::vector<AbiValue> list_v;                    // List
  std::string struct_name;                         // Struct
  std::vector<std::pair<std::string, AbiValue>> struct_fields;  // Struct, decl order
  std::string union_label;                         // Union
  std::vector<std::pair<AbiValue, AbiValue>> map_entries;       // Map, wire order

  AbiValue() = default;
  AbiValue(AbiValue &&) = default;
  AbiValue &operator=(AbiValue &&) = default;
  AbiValue(const AbiValue &) = delete;
  AbiValue &operator=(const AbiValue &) = delete;

  static AbiValue make_int(td::RefInt256 v);
  static AbiValue make_bool(bool v);
  static AbiValue make_address(AbiAddress a);
  static AbiValue make_cell(td::Ref<vm::Cell> c);
  static AbiValue make_cell_of(AbiValue inner_value);
  static AbiValue make_bits(td::Ref<vm::CellSlice> s);
  static AbiValue make_string(std::string s);
  static AbiValue make_list(std::vector<AbiValue> items);
  static AbiValue make_struct(std::string name);
  static AbiValue make_union(std::string label, AbiValue inner_value);
  static AbiValue make_void();
  static AbiValue make_null();
  static AbiValue make_map(std::vector<std::pair<AbiValue, AbiValue>> entries);

  // Appends a field to a Struct-kind value, in call order (= decl order when
  // the walker iterates struct_fields_of in order). No-op / UB on other kinds.
  void add_field(std::string name, AbiValue v);

  // Canonical JSON dump (§0.14), compact (no whitespace). Deterministic key
  // order by construction -- no sorting needed anywhere except what the Map
  // entries already carry from the wire.
  std::string to_json() const;

 private:
  void dump_to(std::string &out) const;
};

}  // namespace ton_abi
