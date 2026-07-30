// Runtime value model for the MCH expression VM (docs/mch-ir-spec.md §7, §12).
//
// This is the C++ mirror of the Python reference values (blocks/utils AccountId
// / Amount / Asset, plus plain int/str/bytes/list/dict) that the expression
// evaluators in indexer/events/mch/expr_eval.py and mch_ir/expr.py produce and
// consume. Only the semantics the conformance vectors exercise are modelled;
// design decisions and deliberate deviations are documented per-member below.
//
// Key decisions:
//  - Integers are td::RefInt256 for EVERY expression integer (Int and Amount
//    value). Python ints are unbounded and jetton amounts overflow int64, so a
//    fixed 256-bit big-int is the faithful carrier. int64 inputs (JSON numbers,
//    host fields) are widened on entry.
//  - AccountId carries a distinct addr_none state (present-but-empty address),
//    separate from a null Value — mirrors AccountId(None): repr "addr_none",
//    as_str() -> null. Addresses are normalized to the raw "wc:HEX" form with
//    UPPERCASE hex (matches AccountId.as_str()); only the raw form is parsed
//    (the vectors never use user-friendly base64 addresses).
//  - Asset == string is JETTON-ONLY: a case-insensitive compare of the jetton
//    master's raw string (Asset.__eq__ lowercases both sides without
//    re-normalizing `other`). A TON asset vs a string FAULTS — __eq__ reads the
//    jetton master before the is_ton branch, so the "ton"-string match is dead.
//  - Dict vs Obj are two distinct map kinds: Dict is pure key lookup (Python
//    dict / block data map / record literal), Obj is attribute access with the
//    special `.exit_code` envelope accessor (Python SimpleNamespace, the $obj
//    decode of a message envelope / body / lookup record). They differ only in
//    attribute-access semantics (§7.2).
//  - List/Dict/Obj payloads are held behind shared_ptr so a Value stays cheaply
//    copyable (values are immutable once built; sharing is safe).
#pragma once

#include "common/refint.h"
#include "vm/cells/Cell.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mch {

struct Block;  // BlockTree.h; a Value can reference a matched block (VType::Block)

enum class VType {
  Null,
  Bool,
  Int,      // td::RefInt256
  Str,      // utf-8 text
  Bytes,    // raw bytes (b64 input)
  Amount,   // td::RefInt256 nanoton-style amount; null num == Python Amount(None)
  Account,  // AccountId (raw addr or addr_none)
  Asset,    // TON | jetton-master
  Cell,     // td::Ref<vm::Cell>
  List,
  Dict,     // pure key lookup
  Obj,      // attribute access + .exit_code
  Block,    // reference to a matched Block (msg/data/failed/broken/btype accessors)
  Float,    // IEEE double from fixture data (Python float); bare -> unrenderable
};

struct Value {
  using Fields = std::vector<std::pair<std::string, Value>>;

  VType t{VType::Null};
  bool boolean{false};
  double dnum{0.0};           // Float, and Amount-of-float (amount_float)
  bool amount_float{false};   // Amount whose value is a Python float (renders %.17g)
  td::RefInt256 num;          // Int, Amount
  std::string str;            // Str (text), Bytes (raw), Account/Asset canonical addr
  bool addr_none{false};      // Account: addr_none marker
  bool is_ton{false};         // Asset: TON asset
  bool has_jetton{false};     // Asset: jetton master present (str holds it)
  td::Ref<vm::Cell> cell;     // Cell
  std::shared_ptr<std::vector<Value>> items;  // List
  std::shared_ptr<Fields> fields;             // Dict / Obj
  const Block *block{nullptr};                // Block (non-owning; arena outlives values)

  // --- factories ---
  static Value null() { return Value{}; }
  static Value make_bool(bool b);
  static Value make_int(td::RefInt256 v);
  static Value make_int64(std::int64_t v);
  static Value make_float(double v);
  static Value make_amount_float(double v);  // Python Amount(float)
  static Value make_str(std::string s);
  static Value make_bytes(std::string raw);
  static Value make_amount(td::RefInt256 v);
  static Value make_amount_none();  // Python Amount(None): == only to itself
  static Value make_block(const Block *b);
  static Value make_account_raw(std::string canonical);  // caller normalized
  static Value make_account_none();
  static Value make_asset_ton();
  static Value make_asset_jetton(std::string canonical_master);
  static Value make_cell(td::Ref<vm::Cell> c);
  static Value make_list(std::vector<Value> xs);
  static Value make_dict(Fields fs);
  static Value make_obj(Fields fs);

  bool is_null() const { return t == VType::Null; }

  // Find a field in a Dict/Obj; nullptr when absent.
  const Value *field(const std::string &name) const;

  // Diagnostic rendering (for vector FAIL lines).
  std::string describe() const;
};

// Serialize a cell tree to standard BOC bytes (td std_boc_serialize, mode 0).
// Plan §12: Cell-derived output fields are compared by ROOT HASH, not bytes, so
// container byte order is no longer significant and the deterministic native
// writer is used everywhere (the former pytoniq-order writer — and its P3
// exponential-re-walk DoS — is gone).
td::Result<std::string> td_boc_serialize(const td::Ref<vm::Cell> &root);

// TODO: remove (harness): the root-hash bar exists so the byte-level A/B dumps
// can ignore BOC serialization provenance. Its only callers are the dump
// renderers (BuildDriver render_value, ParseDump) and --surface; no product path
// reaches it. Goes with the dump renderers. (td_boc_serialize above is NOT part
// of this — host fns and the emu writer use it.)
//
// Plan §12 root-hash parity bar. A produced/parsed field whose value is a
// Cell-derived BOC container (a b64 string from the b64() builtin, or raw BOC
// Bytes from MsgParse) is rendered in the dumps as the cell's ROOT HASH
// ("cellhash:HEX-UPPER") instead of its exact bytes, so serialization-order /
// CRC provenance (pytoniq vs td) is invisible to the byte-level A/B comparison.
// `is_boc_field` gates the render by field name (comment / bitcoin_txid / pubkey
// are raw non-BOC bytes and stay as-is). `boc_field_cellhash` returns the
// cellhash string, or "" if the value is not a decodable BOC (caller falls back
// to the normal render).
bool is_boc_field(const std::string &key);
// The BOC-field name set backing is_boc_field (surfaced by --surface for the twins).
const std::set<std::string> &boc_field_names();
std::string boc_field_cellhash(const Value &v);

// Normalize a raw TON address string "wc:hex64" to canonical "wc:HEX64".
// Returns nullopt for anything that is not a well-formed raw address (the
// vectors never use user-friendly base64). "addr_none" is NOT handled here.
std::optional<std::string> normalize_raw_address(const std::string &s);

// TODO: remove (harness): the vector comparator. Callers are VectorRunner,
// BuildVectorRunner and EmuCelldbLookupTest only — all harness TUs; ExprRuntime
// deliberately keeps its own rt_eq so this stays harness-only (ExprRuntime.cpp:76).
// It lives in Value.cpp purely because that is where Value's internals are.
//
// Structural equality used by the conformance-vector comparator (mirrors the
// Python runner's _equal: type-checked + deep). NOT the `==` operator.
bool structural_equal(const Value &a, const Value &b);

}  // namespace mch
