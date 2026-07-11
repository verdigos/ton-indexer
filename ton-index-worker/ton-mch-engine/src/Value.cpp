#include "Value.h"

#include "td/utils/base64.h"
#include "td/utils/crypto.h"
#include "vm/boc.h"
#include "vm/cells/DataCell.h"

#include <cctype>
#include <list>
#include <map>
#include <set>

namespace mch {

Value Value::make_bool(bool b) {
  Value v;
  v.t = VType::Bool;
  v.boolean = b;
  return v;
}

Value Value::make_int(td::RefInt256 n) {
  Value v;
  v.t = VType::Int;
  v.num = std::move(n);
  return v;
}

Value Value::make_int64(std::int64_t n) {
  return make_int(td::make_refint(n));
}

Value Value::make_float(double n) {
  Value v;
  v.t = VType::Float;
  v.dnum = n;
  return v;
}

Value Value::make_amount_float(double n) {
  Value v;
  v.t = VType::Amount;
  v.amount_float = true;
  v.dnum = n;
  return v;
}

Value Value::make_str(std::string s) {
  Value v;
  v.t = VType::Str;
  v.str = std::move(s);
  return v;
}

Value Value::make_bytes(std::string raw) {
  Value v;
  v.t = VType::Bytes;
  v.str = std::move(raw);
  return v;
}

Value Value::make_amount(td::RefInt256 n) {
  Value v;
  v.t = VType::Amount;
  v.num = std::move(n);
  return v;
}

Value Value::make_amount_none() {
  Value v;
  v.t = VType::Amount;  // num stays null == Python Amount(None)
  return v;
}

Value Value::make_block(const Block *b) {
  Value v;
  v.t = VType::Block;
  v.block = b;
  return v;
}

Value Value::make_account_raw(std::string canonical) {
  Value v;
  v.t = VType::Account;
  v.str = std::move(canonical);
  return v;
}

Value Value::make_account_none() {
  Value v;
  v.t = VType::Account;
  v.addr_none = true;
  return v;
}

Value Value::make_asset_ton() {
  Value v;
  v.t = VType::Asset;
  v.is_ton = true;
  return v;
}

Value Value::make_asset_jetton(std::string canonical_master) {
  Value v;
  v.t = VType::Asset;
  v.is_ton = false;
  v.has_jetton = true;
  v.str = std::move(canonical_master);
  return v;
}

Value Value::make_cell(td::Ref<vm::Cell> c) {
  Value v;
  v.t = VType::Cell;
  v.cell = std::move(c);
  return v;
}

Value Value::make_list(std::vector<Value> xs) {
  Value v;
  v.t = VType::List;
  v.items = std::make_shared<std::vector<Value>>(std::move(xs));
  return v;
}

Value Value::make_dict(Fields fs) {
  Value v;
  v.t = VType::Dict;
  v.fields = std::make_shared<Fields>(std::move(fs));
  return v;
}

Value Value::make_obj(Fields fs) {
  Value v;
  v.t = VType::Obj;
  v.fields = std::make_shared<Fields>(std::move(fs));
  return v;
}

const Value *Value::field(const std::string &name) const {
  if (!fields) {
    return nullptr;
  }
  for (const auto &kv : *fields) {
    if (kv.first == name) {
      return &kv.second;
    }
  }
  return nullptr;
}

td::Result<std::string> td_boc_serialize(const td::Ref<vm::Cell> &root) {
  // Plan §12: the dumps render Cell-derived fields by ROOT HASH, so BOC
  // container byte order no longer matters — use td's deterministic native
  // writer (mode 0) everywhere. Replaces the former pytoniq-order writer (and
  // with it the P3 exponential-re-walk DoS).
  if (root.is_null()) {
    return td::Status::Error("td_boc_serialize: null cell");
  }
  auto r = vm::std_boc_serialize(root, 0);
  if (r.is_error()) {
    return r.move_as_error();
  }
  return r.move_as_ok().as_slice().str();
}

bool is_boc_field(const std::string &key) {
  // Cell-derived BOC container fields (plan §12). `comment` / `bitcoin_txid` /
  // `pubkey` are raw non-BOC bytes and are DELIBERATELY excluded.
  static const std::set<std::string> kBocFields = {
      "forward_payload", "custom_payload", "message_boc", "message_boc_str",
      "order_boc",       "order_boc_str",  "stonfi_swap_body",
  };
  return kBocFields.count(key) != 0;
}

std::string boc_field_cellhash(const Value &v) {
  auto hash_of = [](const td::Ref<vm::Cell> &cell) -> std::string {
    auto h = cell->get_hash();
    static const char *hexd = "0123456789ABCDEF";
    std::string out;
    const unsigned char *p = h.as_slice().ubegin();
    for (std::size_t i = 0; i < h.as_slice().size(); i++) {
      out.push_back(hexd[p[i] >> 4]);
      out.push_back(hexd[p[i] & 0xF]);
    }
    return "cellhash:" + out;
  };
  // Decode-fail marker (never throws — the dump must not abort/trace-fail on a
  // bad BOC field). A short deterministic prefix aids diagnosis; both engines
  // compute it identically from the same value type (str → the b64 string;
  // bytes → base64 of the raw), so a fail-case would still be A/B-comparable.
  // Never fires over the corpus (all BOC fields decode) — defensive only.
  auto decode_fail = [](const std::string &enc_prefix_src) {
    return "cellhash:DECODE_FAIL:" + enc_prefix_src.substr(0, 12);
  };
  if (v.t == VType::Cell) {
    return v.cell.is_null() ? std::string() : hash_of(v.cell);  // null cell → fall back
  }
  std::string raw;
  if (v.t == VType::Bytes) {
    raw = v.str;  // raw BOC bytes (MsgParse payload)
    auto r_cell = vm::std_boc_deserialize(td::Slice(raw));
    if (r_cell.is_error() || r_cell.ok().is_null()) {
      return decode_fail(td::base64_encode(td::Slice(raw)));
    }
    return hash_of(r_cell.move_as_ok());
  }
  if (v.t == VType::Str) {
    auto r = td::base64_decode(v.str);  // b64() builtin output
    if (r.is_error()) {
      return decode_fail(v.str);
    }
    auto r_cell = vm::std_boc_deserialize(td::Slice(r.ok()));
    if (r_cell.is_error() || r_cell.ok().is_null()) {
      return decode_fail(v.str);
    }
    return hash_of(r_cell.move_as_ok());
  }
  return std::string();  // null / other type → fall back to normal render
}

std::optional<std::string> normalize_raw_address(const std::string &s) {
  auto colon = s.find(':');
  if (colon == std::string::npos || colon == 0) {
    return std::nullopt;
  }
  const std::string wc_part = s.substr(0, colon);
  const std::string hex_part = s.substr(colon + 1);

  // Workchain: optional leading '-', then digits.
  std::size_t p = 0;
  if (wc_part[p] == '-') {
    p++;
  }
  if (p >= wc_part.size()) {
    return std::nullopt;
  }
  for (; p < wc_part.size(); p++) {
    if (!std::isdigit(static_cast<unsigned char>(wc_part[p]))) {
      return std::nullopt;
    }
  }
  // 256-bit account id == exactly 64 hex digits.
  if (hex_part.size() != 64) {
    return std::nullopt;
  }
  std::string up;
  up.reserve(64);
  for (char c : hex_part) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) {
      return std::nullopt;
    }
    up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return wc_part + ":" + up;
}

bool structural_equal(const Value &a, const Value &b) {
  // Mirrors the Python vector runner's _equal: expected type drives the check,
  // actual must match that type and be deeply equal.
  switch (b.t) {
    case VType::Null:
      return a.t == VType::Null;
    case VType::Bool:
      return a.t == VType::Bool && a.boolean == b.boolean;
    case VType::Int:
      return a.t == VType::Int && cmp(a.num, b.num) == 0;
    case VType::Amount:
      if (a.t != VType::Amount) {
        return false;
      }
      if (a.num.is_null() || b.num.is_null()) {
        return a.num.is_null() && b.num.is_null();  // Amount(None) == Amount(None)
      }
      return cmp(a.num, b.num) == 0;
    case VType::Str:
      return a.t == VType::Str && a.str == b.str;
    case VType::Bytes:
      return a.t == VType::Bytes && a.str == b.str;
    case VType::Account:
      if (a.t != VType::Account) {
        return false;
      }
      if (a.addr_none || b.addr_none) {
        return a.addr_none && b.addr_none;
      }
      return a.str == b.str;
    case VType::Asset:
      if (a.t != VType::Asset || a.is_ton != b.is_ton || a.has_jetton != b.has_jetton) {
        return false;
      }
      return !a.has_jetton || a.str == b.str;
    case VType::Cell: {
      if (a.t != VType::Cell) {
        return false;
      }
      if (a.cell.is_null() || b.cell.is_null()) {
        return a.cell.is_null() && b.cell.is_null();
      }
      return a.cell->get_hash() == b.cell->get_hash();
    }
    case VType::List: {
      if (a.t != VType::List) {
        return false;
      }
      const auto &xs = *a.items;
      const auto &ys = *b.items;
      if (xs.size() != ys.size()) {
        return false;
      }
      for (std::size_t i = 0; i < xs.size(); i++) {
        if (!structural_equal(xs[i], ys[i])) {
          return false;
        }
      }
      return true;
    }
    case VType::Block:
      // Python Block.__eq__ is object identity.
      return a.t == VType::Block && a.block == b.block;
    case VType::Dict:
    case VType::Obj: {
      if (a.t != b.t || !a.fields || !b.fields) {
        return false;
      }
      const auto &af = *a.fields;
      const auto &bf = *b.fields;
      if (af.size() != bf.size()) {
        return false;
      }
      // Set-of-keys comparison (order-independent), each key deep-equal.
      for (const auto &kv : bf) {
        const Value *av = a.field(kv.first);
        if (av == nullptr || !structural_equal(*av, kv.second)) {
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

std::string Value::describe() const {
  switch (t) {
    case VType::Null:
      return "null";
    case VType::Bool:
      return boolean ? "true" : "false";
    case VType::Int:
      return "int(" + (num.is_null() ? std::string("nan") : dec_string(num)) + ")";
    case VType::Amount:
      return "amount(" + (num.is_null() ? std::string("nan") : dec_string(num)) + ")";
    case VType::Str:
      return "str(" + str + ")";
    case VType::Bytes:
      return "bytes[" + std::to_string(str.size()) + "]";
    case VType::Account:
      return addr_none ? "addr_none" : ("account(" + str + ")");
    case VType::Asset:
      return is_ton ? "asset(TON)" : ("asset(" + str + ")");
    case VType::Cell:
      return "cell";
    case VType::Block:
      return "Block";
    case VType::List: {
      std::string out = "[";
      for (std::size_t i = 0; i < items->size(); i++) {
        if (i) {
          out += ", ";
        }
        out += (*items)[i].describe();
      }
      return out + "]";
    }
    case VType::Dict:
    case VType::Obj: {
      std::string out = (t == VType::Obj ? "obj{" : "{");
      bool first = true;
      for (const auto &kv : *fields) {
        if (!first) {
          out += ", ";
        }
        first = false;
        out += kv.first + ": " + kv.second.describe();
      }
      return out + "}";
    }
  }
  return "?";
}

}  // namespace mch
