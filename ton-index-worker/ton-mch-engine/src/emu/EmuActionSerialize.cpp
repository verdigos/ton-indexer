#include "EmuActionSerialize.h"

#include "Value.h"

#include "td/utils/base64.h"

#include <msgpack.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <system_error>
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

// §2.7 presence rule, TOP LEVEL ONLY: the Value shapes that mean "this Action
// field was never filled". Python reaches the same wire state by never
// assigning the attribute — an unassigned column is simply not a key in
// Action.to_dict() (§2.1), and AccountId.as_str(), Amount.value and a TON Asset
// all yield None.
//
// It deliberately does NOT apply one level down: a composite is a literal dict
// whose key set is fixed by its fill (block_tree_serializer.py:195-204 assigns
// all seven jetton_transfer_data keys, the None ones included), so an unfilled
// composite field is present-as-nil on Python's wire. pack_fields keeps every
// key for exactly that reason.
bool omitted(const Value &v) {
  switch (v.t) {
    case VType::Null:
      return true;
    case VType::Account:
      return v.addr_none;
    case VType::Asset:
      return v.is_ton || !v.has_jetton;
    case VType::Amount:
      return !v.amount_float && v.num.is_null();  // make_amount_none()
    default:
      return false;
  }
}

// Python str(float) == repr(float), which is NOT plain shortest round-trip.
// CPython (pystrtod.c format_float_short, 'r' mode) renders the shortest
// round-trip digits in FIXED notation whenever the decimal exponent sits in
// (-4, 16], always with a fractional part ("1037.0", never "1037"), and
// switches to exponential outside that window with a >=2-digit exponent
// ("1e+16", "1e-05").
//
// std::to_chars' default would emit "1037" and "1.03e+11". Both decode — every
// Go field that can carry one is *string — but they are DIFFERENT TEXT for an
// API consumer, and Python is the incumbent writer. Found by the W2 parity gate
// on nft_transfer_data.{price,nft_item_index}, which contradicts §2.7's "no
// ported fill produces one today".
std::string float_text(double d) {
  if (std::isnan(d)) return "nan";
  if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
  char buf[64];
  auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), d, std::chars_format::scientific);
  if (ec != std::errc()) return "0.0";
  // "[-]D.DDDDe±XX" -> sign, bare digit string, decimal exponent.
  const std::string s(buf, end);
  const std::string sign = s[0] == '-' ? "-" : "";
  const std::size_t epos = s.find('e');
  std::string digits = s.substr(sign.size(), epos - sign.size());
  digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end());
  const int exp10 = std::stoi(s.substr(epos + 1));
  const int decpt = exp10 + 1;  // value == 0.<digits> * 10^decpt

  if (decpt <= -4 || decpt > 16) {
    std::string mantissa(1, digits[0]);
    if (digits.size() > 1) mantissa += "." + digits.substr(1);
    std::string e = std::to_string(exp10 < 0 ? -exp10 : exp10);
    if (e.size() < 2) e = "0" + e;
    return sign + mantissa + (exp10 < 0 ? "e-" : "e+") + e;
  }
  if (decpt <= 0) {
    return sign + "0." + std::string(static_cast<std::size_t>(-decpt), '0') + digits;
  }
  if (static_cast<std::size_t>(decpt) >= digits.size()) {
    return sign + digits + std::string(decpt - digits.size(), '0') + ".0";
  }
  return sign + digits.substr(0, decpt) + "." + digits.substr(decpt);
}

// An Int that stays a msgpack INTEGER: Python's top-level stringify exclusion
// set (§2.2, here only `opcode`) and the §2.6/Q1 divergence keys inside the
// composites. Out of int64 range packs nil rather than truncating silently.
void pack_int(Packer &pk, const Value &v, ActionSerializeStats &st) {
  if (v.num.is_null() || !v.num->fits_bits(64)) {
    st.unrenderable++;
    pk.pack_nil();
    return;
  }
  pk.pack(static_cast<std::int64_t>(v.num->to_long()));
}

void pack_value(Packer &pk, const Value &v, const std::set<std::string> *int_keys,
                ActionSerializeStats &st);

// Dict/Obj -> msgpack map in INSERTION order (the composite payloads). EVERY
// key is written, an unfilled one as nil: see omitted() — the composites are
// literal dicts on the Python side, so absence is not a state they can express.
//
// A key in `int_keys` keeps its Int as an integer: §2.6/Q1, where Python's
// top-level-only exclusion set stringifies a field ton-index-go declares as
// *int32/*int64, and the resulting type error drops the WHOLE trace. The
// divergence sets are flat, so nested maps are packed with no exceptions.
void pack_fields(Packer &pk, const Value::Fields &fs, const std::set<std::string> *int_keys,
                 ActionSerializeStats &st) {
  pk.pack_map(static_cast<std::uint32_t>(fs.size()));
  for (const auto &[key, val] : fs) {
    pack_text(pk, key);
    if (int_keys != nullptr && val.t == VType::Int && int_keys->count(key) != 0) {
      pack_int(pk, val, st);
    } else {
      pack_value(pk, val, nullptr, st);
    }
  }
}

void pack_value(Packer &pk, const Value &v, const std::set<std::string> *int_keys,
                ActionSerializeStats &st) {
  switch (v.t) {
    case VType::Null:
      // omitted() gates every map call site; reachable only from a List, where
      // position is significant and the element cannot be dropped.
      pk.pack_nil();
      return;
    case VType::Bool:
      pk.pack(v.boolean);  // §2.2: database.py never stringifies a bool
      return;
    case VType::Int:
      if (v.num.is_null()) {
        st.unrenderable++;
        pk.pack_nil();
        return;
      }
      pack_text(pk, v.num->to_dec_string());  // §2.2: every non-excluded int is a str
      return;
    case VType::Float:
      st.float_values++;
      pack_text(pk, float_text(v.dnum));
      return;
    case VType::Amount:
      if (v.amount_float) {
        st.float_values++;
        pack_text(pk, float_text(v.dnum));
        return;
      }
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
      // Python holds these as base64 str already (jettons.py custom_payload /
      // forward_payload); the C++ side carries the raw bytes.
      pack_text(pk, td::base64_encode(td::Slice(v.str)));
      return;
    case VType::Cell: {
      // §2.7: should never reach an Action — the BOC-field root-hash
      // substitution is a twin-diff device and must NOT be reused here, so the
      // real bytes go out base64'd and the counter makes the sighting visible.
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
        pack_value(pk, item, nullptr, st);
      }
      return;
    case VType::Dict:
    case VType::Obj:
      pack_fields(pk, *v.fields, int_keys, st);
      return;
    case VType::Block:
      // Nulled before the rows leave the classifier actor
      // (EmuClassifierActor.cpp:44-63); a survivor holds a dead arena pointer,
      // so it is never dereferenced here.
      st.unrenderable++;
      pk.pack_nil();
      return;
  }
  pk.pack_nil();
}

struct ActionField {
  const char *key;
  Value Action::*member;
  // §2.2: `opcode` is in Python's stringify exclusion set, so its Int ships as
  // a msgpack integer rather than a decimal string.
  bool self_int;
  // §2.6/Q1: keys INSIDE this composite whose Int stays a msgpack integer.
  const std::set<std::string> *int_keys;
};

// The Value-typed Action fields, each omitted when unfilled. Same set and order
// as scrub_arena_refs (EmuClassifierActor.cpp:65-77).
const std::vector<ActionField> &optional_fields() {
  static const std::set<std::string> jvault_stake_ints{"period"};  // Go *int64
  static const std::set<std::string> layerzero_packet_ints{
      "src_eid", "dst_eid", "nonce"};  // Go *int32, *int32, *int64
  static const std::vector<ActionField> fields{
      {"source", &Action::source, false, nullptr},
      {"source_secondary", &Action::source_secondary, false, nullptr},
      {"destination", &Action::destination, false, nullptr},
      {"destination_secondary", &Action::destination_secondary, false, nullptr},
      {"asset", &Action::asset, false, nullptr},
      {"asset_secondary", &Action::asset_secondary, false, nullptr},
      {"asset2", &Action::asset2, false, nullptr},
      {"asset2_secondary", &Action::asset2_secondary, false, nullptr},
      {"amount", &Action::amount, false, nullptr},
      {"value", &Action::value, false, nullptr},
      {"opcode", &Action::opcode, true, nullptr},
      {"ton_transfer_data", &Action::ton_transfer_data, false, nullptr},
      {"jetton_transfer_data", &Action::jetton_transfer_data, false, nullptr},
      {"jetton_swap_data", &Action::jetton_swap_data, false, nullptr},
      {"nft_transfer_data", &Action::nft_transfer_data, false, nullptr},
      {"nft_listing_data", &Action::nft_listing_data, false, nullptr},
      {"dex_deposit_liquidity_data", &Action::dex_deposit_liquidity_data, false, nullptr},
      {"dex_withdraw_liquidity_data", &Action::dex_withdraw_liquidity_data, false, nullptr},
      {"staking_data", &Action::staking_data, false, nullptr},
      {"evaa_withdraw_data", &Action::evaa_withdraw_data, false, nullptr},
      {"vesting_send_message_data", &Action::vesting_send_message_data, false, nullptr},
      {"layerzero_packet_data", &Action::layerzero_packet_data, false, &layerzero_packet_ints},
      {"jvault_stake_data", &Action::jvault_stake_data, false, &jvault_stake_ints},
      {"jvault_claim_data", &Action::jvault_claim_data, false, nullptr},
      {"change_dns_record_data", &Action::change_dns_record_data, false, nullptr},
      {"extra", &Action::extra, false, nullptr},
  };
  return fields;
}

// Number of always-written keys below. Deliberately NOT written (§2.3):
// trace_id / trace_external_hash / trace_end_lt / trace_end_utime /
// trace_mc_seqno_end, which Go clobbers right after decode
// (models.go:830-836); trace_start_lt and value_extra_currencies, which Python
// never assigns on this path and Go has no field for. Omitting them also
// removes eleven chances to get a wire type wrong.
constexpr std::uint32_t kFixedKeys = 12;

void pack_action(Packer &pk, const Action &a, const std::string &trace_key, std::int64_t finality,
                 ActionSerializeStats &st) {
  const std::vector<ActionField> &opt = optional_fields();
  std::uint32_t n = kFixedKeys;
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
  // §2.2 exclusion set: these five stay msgpack integers.
  pack_key(pk, "start_lt");
  pk.pack(a.start_lt);
  pack_key(pk, "end_lt");
  pk.pack(a.end_lt);
  pack_key(pk, "start_utime");
  pk.pack(a.start_utime);
  pack_key(pk, "end_utime");
  pk.pack(a.end_utime);
  // Python writes mc_seqno_end and Go ignores it; kept for parity, one key.
  pack_key(pk, "mc_seqno_end");
  pk.pack(a.mc_seqno_end);
  // Not on mch::Action — both come from the view (§2.5).
  pack_key(pk, "finality");
  pk.pack(finality);
  // Go only surfaces it when it differs from trace_external_hash
  // (models.go:930), so it must be written.
  pack_key(pk, "trace_external_hash_norm");
  pack_text(pk, trace_key);

  for (const ActionField &f : opt) {
    const Value &v = a.*f.member;
    if (omitted(v)) {
      continue;
    }
    pack_key(pk, f.key);
    if (f.self_int) {
      pack_int(pk, v, st);
    } else {
      pack_value(pk, v, f.int_keys, st);
    }
  }
}

}  // namespace

std::string serialize_actions(const std::vector<Action> &actions, const EmuTraceView &view,
                              ActionSerializeStats *stats) {
  ActionSerializeStats discarded;
  ActionSerializeStats &st = stats != nullptr ? *stats : discarded;
  // Python's min(tx.finality) over the trace. The emulator's 3-value enum
  // {emulated,confirmed,finalized} is exactly Go's FinalityState
  // {pending,confirmed,finalized}, and an empty view degrades to the
  // conservative end. W5 moved the derivation into EmuTypes.h because the §4.3a
  // insert guard needs the SAME value — two copies would let the finality a
  // trace is guarded on drift from the one its own payload advertises.
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
