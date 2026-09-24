#include "RedisActionSerialize.h"

#include "ActionPayload.h"
#include "Value.h"

#include "td/utils/base64.h"

#include <msgpack.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mch {

namespace {

using Packer = msgpack::packer<msgpack::sbuffer>;

void pack_key(Packer &pk, const char *k) {
  const auto n = static_cast<std::uint32_t>(std::strlen(k));
  pk.pack_str(n);
  pk.pack_str_body(k, n);
}

void pack_text(Packer &pk, const std::string &s) {
  const auto n = static_cast<std::uint32_t>(s.size());
  pk.pack_str(n);
  pk.pack_str_body(s.data(), n);
}

void pack_binary(Packer &pk, const std::string &s) {
  const auto n = static_cast<std::uint32_t>(s.size());
  pk.pack_bin(n);
  pk.pack_bin_body(s.data(), n);
}

// These shapes omit an unfilled row column. Composite fallback values retain
// their full key set and encode unfilled members as nil.
bool omitted(const Value &v) {
  switch (v.t) {
    case VType::Null:
      return true;
    case VType::Amount:
      return v.num.is_null();  // make_amount_none()
    case VType::Bool:
    case VType::Int:
    case VType::Str:
    case VType::Account:
    case VType::Asset:
    case VType::Bytes:
    case VType::Cell:
    case VType::List:
    case VType::Dict:
    case VType::Obj:
    case VType::Block:
      return false;
  }
}

// Row amounts can exceed int64 and stay decimal strings. Bounded row integers
// use native msgpack types.
bool decimal_string_key(const std::string &k) {
  return k == "amount" || k == "value";
}

// Native integer fields must fit signed int64. Encode an unexpected wider value
// as nil rather than truncating it or invalidating the complete Go decode.
void pack_int(Packer &pk, const Value &v, RedisActionSerializeStats &st) {
  if (v.num.is_null() || !v.num->fits_bits(64)) {
    st.unrenderable++;
    pk.pack_nil();
    return;
  }
  pk.pack(static_cast<std::int64_t>(v.num->to_long()));
}

void pack_value(Packer &pk, const Value &v, bool as_decimal_string, RedisActionSerializeStats &st);

// Encode composite maps in insertion order and retain unfilled keys as nil.
void pack_fields(Packer &pk, const Value::Fields &fs, RedisActionSerializeStats &st) {
  pk.pack_map(static_cast<std::uint32_t>(fs.size()));
  for (const auto &[key, val] : fs) {
    pack_text(pk, key);
    pack_value(pk, val, decimal_string_key(key), st);
  }
}

void pack_value(Packer &pk, const Value &v, bool as_decimal_string, RedisActionSerializeStats &st) {
  switch (v.t) {
    case VType::Null:
      // omitted() gates every top-level call site; reachable from a composite
      // key (present-as-nil) and from a List, where position is significant and
      // the element cannot be dropped.
      pk.pack_nil();
      return;
    case VType::Bool:
      pk.pack(v.boolean);
      return;
    case VType::Int:
      if (v.num.is_null()) {
        st.unrenderable++;
        pk.pack_nil();
        return;
      }
      if (as_decimal_string) {
        pack_text(pk, v.num->to_dec_string());
      } else {
        pack_int(pk, v, st);
      }
      return;
    case VType::Amount:
      // Value-domain by definition, so always a decimal string regardless of
      // the key. (ActionBuild's av_amount converts Amount -> Int before it ever
      // reaches a fill's output, so this arm is a backstop.)
      if (v.num.is_null()) {
        pk.pack_nil();
        return;
      }
      pack_text(pk, v.num->to_dec_string());
      return;
    case VType::Str:
      pack_text(pk, v.str);
      return;
    case VType::Account:
      if (v.addr_none) {
        pk.pack_nil();
        return;
      }
      pack_text(pk, v.str);  // canonical wc:HEXUPPER, == AccountId.as_str()
      return;
    case VType::Asset:
      if (v.is_ton || !v.has_jetton) {
        pk.pack_nil();
        return;
      }
      pack_text(pk, v.str);
      return;
    case VType::Bytes:
      // Bytes fields are encoded as base64 text.
      pack_text(pk, td::base64_encode(td::Slice(v.str)));
      return;
    case VType::Cell: {
      // Preserve unexpected cells as base64 BOCs and count the occurrence.
      st.cell_values++;
      if (v.cell.is_null()) {
        st.unrenderable++;
        pk.pack_nil();
        return;
      }
      auto boc = td_boc_serialize(v.cell);
      if (boc.is_error()) {
        st.unrenderable++;
        pk.pack_nil();
        return;
      }
      pack_text(pk, td::base64_encode(td::Slice(boc.ok())));
      return;
    }
    case VType::List:
      pk.pack_array(static_cast<std::uint32_t>(v.items->size()));
      for (const Value &item : *v.items) {
        pack_value(pk, item, as_decimal_string, st);  // elements inherit the key's decision
      }
      return;
    case VType::Dict:
    case VType::Obj:
      pack_fields(pk, *v.fields, st);
      return;
    case VType::Block:
      // Nulled before the rows leave the classifier actor (scrub_arena_refs);
      // a survivor holds a dead arena pointer, so it is never dereferenced here.
      st.unrenderable++;
      pk.pack_nil();
      return;
  }
}

struct ActionField {
  const char *key;
  Value Action::*member;
};

const std::vector<ActionField> &optional_fields() {
  static const std::vector<ActionField> fields{
      {"source", &Action::source},
      {"source_secondary", &Action::source_secondary},
      {"destination", &Action::destination},
      {"destination_secondary", &Action::destination_secondary},
      {"asset", &Action::asset},
      {"asset_secondary", &Action::asset_secondary},
      {"asset2", &Action::asset2},
      {"asset2_secondary", &Action::asset2_secondary},
      {"amount", &Action::amount},
      {"value", &Action::value},
      {"opcode", &Action::opcode},
  };
  return fields;
}

// Number of unconditional keys below. ancestor_type and optional fields are
// counted separately; fields unused by the Go consumer are not serialized.
constexpr std::uint32_t kFixedKeys = 12;

void pack_action(Packer &pk, const Action &a, const std::string &trace_key, std::int64_t finality,
                 RedisActionSerializeStats &st) {
  const std::vector<ActionField> &opt = optional_fields();
  std::uint32_t n = kFixedKeys + (a.ancestor_type.empty() ? 0 : 1);
  for (const ActionField &f : opt) {
    if (!omitted(a.*f.member)) {
      n++;
    }
  }
  pk.pack_map(n);

  pack_key(pk, "action_id");
  pack_text(pk, a.action_id);
  pack_key(pk, "type");
  pack_text(pk, a.type);
  pack_key(pk, "success");
  pk.pack(a.success);
  // []string on the Go side, where element decoding bypasses the flexible
  // decoders: nothing numeric may ever be written into one of these arrays.
  pack_key(pk, "tx_hashes");
  pk.pack_array(static_cast<std::uint32_t>(a.tx_hashes.size()));
  for (const std::string &h : a.tx_hashes) {
    pack_text(pk, h);
  }
  pack_key(pk, "accounts");
  pk.pack_array(static_cast<std::uint32_t>(a.accounts.size()));
  for (const std::string &acc : a.accounts) {
    pack_text(pk, acc);
  }
  pack_key(pk, "start_lt");
  pk.pack(a.start_lt);
  pack_key(pk, "end_lt");
  pk.pack(a.end_lt);
  pack_key(pk, "start_utime");
  pk.pack(a.start_utime);
  pack_key(pk, "end_utime");
  pk.pack(a.end_utime);
  // Both fields come from the view rather than mch::Action.
  pack_key(pk, "finality");
  pk.pack(finality);
  // Go only surfaces it when it differs from trace_external_hash
  // (models.go:930), so it must be written.
  pack_key(pk, "trace_external_hash_norm");
  pack_text(pk, trace_key);
  // ancestor_type is present only for child rows.
  if (!a.ancestor_type.empty()) {
    pack_key(pk, "ancestor_type");
    pk.pack_array(static_cast<std::uint32_t>(a.ancestor_type.size()));
    for (const std::string &t : a.ancestor_type) {
      pack_text(pk, t);
    }
  }
  pack_key(pk, "payload");
  pack_binary(pk, encode_action_payload(a));

  for (const ActionField &f : opt) {
    const Value &v = a.*f.member;
    if (omitted(v)) {
      continue;
    }
    pack_key(pk, f.key);
    pack_value(pk, v, decimal_string_key(f.key), st);
  }
}

}  // namespace

std::string serialize_redis_actions(const std::vector<Action> &actions, const EmuTraceView &view,
                                   RedisActionSerializeStats *stats) {
  RedisActionSerializeStats discarded;
  RedisActionSerializeStats &st = stats != nullptr ? *stats : discarded;
  // The payload and insert guard share the minimum node finality.
  const auto finality = static_cast<std::int64_t>(view_finality(view));

  msgpack::sbuffer buf;
  Packer pk(&buf);
  pk.pack_array(static_cast<std::uint32_t>(actions.size()));
  for (const Action &a : actions) {
    pack_action(pk, a, view.trace_id, finality, st);
  }
  return std::string(buf.data(), buf.size());
}

}  // namespace mch
