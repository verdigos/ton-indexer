#include "RedisActionSerialize.h"

#include "ActionPayload.h"
#include "Value.h"

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

// Row amounts can exceed int64 and stay decimal strings. Bounded row integers
// use native msgpack types.
bool decimal_string_key(const std::string &k) {
  return k == "amount" || k == "value";
}

void pack_value(Packer &pk, const Value &v, bool as_decimal_string) {
  if (v.t == VType::Str) {
    pack_text(pk, v.str);
  } else if (as_decimal_string) {
    pack_text(pk, v.num->to_dec_string());
  } else {
    pk.pack(static_cast<std::int64_t>(v.num->to_long()));
  }
}

struct ActionField {
  const char *key;
  Value Action::*member;
};

const std::vector<ActionField> &optional_fields() {
  static const std::vector<ActionField> fields{
#define MCH_ACTION_FIELD(name) {#name, &Action::name},
      MCH_ACTION_COLUMNS(MCH_ACTION_FIELD)
#undef MCH_ACTION_FIELD
  };
  return fields;
}

// Number of unconditional keys below. ancestor_type and optional fields are
// counted separately; fields unused by the Go consumer are not serialized.
constexpr std::uint32_t kFixedKeys = 12;

void pack_action(Packer &pk, const Action &a, const std::string &trace_key, std::int64_t finality) {
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
  // Go surfaces this value when it differs from trace_external_hash.
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
  pack_binary(pk, action_payload_bytes(a));

  for (const ActionField &f : opt) {
    const Value &v = a.*f.member;
    if (omitted(v)) {
      continue;
    }
    pack_key(pk, f.key);
    pack_value(pk, v, decimal_string_key(f.key));
  }
}

}  // namespace

std::string serialize_redis_actions(const std::vector<Action> &actions, const EmuTraceView &view) {
  // The payload and insert guard share the minimum node finality.
  const auto finality = static_cast<std::int64_t>(view_finality(view));

  msgpack::sbuffer buf;
  Packer pk(&buf);
  pk.pack_array(static_cast<std::uint32_t>(actions.size()));
  for (const Action &a : actions) {
    pack_action(pk, a, view.trace_id, finality);
  }
  return std::string(buf.data(), buf.size());
}

}  // namespace mch
