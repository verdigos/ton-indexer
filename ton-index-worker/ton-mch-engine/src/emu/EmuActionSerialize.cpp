#include "EmuActionSerialize.h"

#include "Value.h"

#include "td/utils/base64.h"

#include <msgpack.hpp>

#include <cstdint>
#include <cstring>
#include <set>
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

// Presence rule, TOP LEVEL ONLY: the Value shapes that mean "this Action field
// was never filled". Python reaches the same wire state by never assigning the
// attribute — an unassigned column is simply not a key in Action.to_dict(), and
// AccountId.as_str(), Amount.value and a TON Asset all yield None.
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

// THE NATURAL-TYPES RULE.
//
// Emit the natural msgpack type. A value-domain field that can exceed int64 is
// a DECIMAL STRING, because it does not fit an integer — a value-domain fact,
// not a Python quirk. ton-index-go accepts both directions for every scalar
// field (index/emulated/flexdecode.go registers flexible decoders for the ten
// integer kinds AND for string), so nothing here has to reproduce Python's
// stringify rules; the only thing that must be right is which fields cannot be
// represented as an int64 at all.
//
// This is the whole set, matched by KEY NAME at every map depth (a list element
// inherits its key's decision — jvault_claim_data.claimed_amounts is a list of
// decimal strings). Flat rather than per-composite because no key name in this
// set is ever a bounded integer somewhere else; the decision only ever applies
// to a VType::Int, so a key like change_dns_record_data's `value` (always Str
// or Null) is untouched by its presence here.
//
// `claimed_amounts` is doubly required: 256-bit, AND it is a Go []string whose
// ELEMENTS bypass the flexible decoder (msgpack-go's slice fast path never
// consults a registered element decoder). The same holds for every other
// []string-valued field — tx_hashes, accounts, ancestor_type, claimed_jettons —
// which is why nothing numeric may ever be emitted inside one.
//
// Deliberately NOT here, i.e. emitted as natural msgpack integers: `opcode`
// (Go *uint32), `flags` (uint8 DNS record flags, Go *int64), `period` (Go
// *int64), `src_eid`/`dst_eid` (int32), `tick_lower`/`tick_upper`
// (int24), `exit_code`, and the already-native start_lt / end_lt / start_utime
// / end_utime / finality. `evaa_withdraw_data.asset_id` is a hex STRING built
// by av_hex and must not be renaturalized.
bool decimal_string_key(const std::string &k) {
  static const std::set<std::string> keys{
      // top level
      "amount", "value",
      // jetton_transfer_data / vesting_send_message_data / nft_transfer_data
      "forward_amount", "query_id",
      // jetton_swap_data (+ its nested transfer / peer-swap maps)
      "min_out_amount", "amount_in", "amount_out",
      // nft_transfer_data
      "price", "payout_amount", "royalty_amount", "nft_item_index",
      // dex_deposit_liquidity_data / dex_withdraw_liquidity_data
      // (`amount` above also covers vault_excesses[].amount)
      "amount1", "amount2", "lp_tokens_minted", "lp_tokens_burnt",
      "target_amount_1", "target_amount_2", "nft_index",
      // staking_data
      "tokens_minted", "tokens_burnt",
      // coffee_create_pool_data (the underscore spelling is this composite's
      // alone; `lp_tokens_minted` above covers its sixth field) /
      // coffee_staking_*_data. All six Go fields are *string; these four are the
      // ones that can exceed int64 — two Coins amounts, an nft index and a
      // Coins-width points balance. `nft_index` above covers the withdraw side.
      "amount_1", "amount_2", "minted_item_index", "points",
      // jvault_claim_data — a Go []string, see above
      "claimed_amounts",
      // layerzero_send_data. `native_fee`/`zro_fee` are uint128 off the lzSend
      // payload and `send_request_id` is a u64 counter — none of the three fits
      // a signed int64 by construction, and all three are Go *uint64. The
      // packet's `nonce` / `src_eid` / `dst_eid` are bounded and stay natural
      // integers (see the exclusion list above), as does dvn_verify's `nonce`.
      "send_request_id", "native_fee", "zro_fee",
      // tonco_deploy_pool_data. All ten Go fields are *string, but only the
      // Q64.96 sqrt price cannot be an int64 — it is a uint160 by construction.
      // The three fee fields and `tick_spacing` are u16/int24 and stay natural
      // integers, like `tick_lower`/`tick_upper` above.
      "initial_price_x96",
      // vesting_add_whitelist_data.accounts_added is a Go []string of ADDRESSES
      // (never numeric), so it needs no entry here; `query_id` above covers the
      // composite's only integer field.
      // The cocoon composites. Six unbounded u64/coins fields, all Go *string.
      // `nonce` MOVED here from the exclusion list above when cocoon landed:
      // cocoon_client_register's nonce is a raw u64 that routinely exceeds
      // int64 (the 5B6Ex8kv fixture carries 11924145372215500834), so it cannot
      // stay a natural integer. LayerZero's bounded `nonce` (Go *int64) now
      // travels as a decimal string too — lossless, since decodeFlexInt parses
      // it back. `worker_state` (u2) and `seqno` (u32) stay natural integers,
      // and `payout_type` / `expected_address` / `new_secret_hash` /
      // `via_wallet` are not integers at all.
      "nonce", "new_tokens", "worker_tokens", "new_tokens_used", "new_stake",
      "withdraw_amount",
  };
  return keys.count(k) != 0;
}

// An Int that stays a msgpack INTEGER. Out of SIGNED int64 range packs nil
// rather than truncating silently: every field that can carry such a value is
// in decimal_string_key() by construction, so this firing means the key table
// is wrong — and an absent field is the only failure mode that cannot drop the
// whole trace on the Go side.
void pack_int(Packer &pk, const Value &v, ActionSerializeStats &st) {
  if (v.num.is_null() || !v.num->fits_bits(64)) {
    st.unrenderable++;
    pk.pack_nil();
    return;
  }
  pk.pack(static_cast<std::int64_t>(v.num->to_long()));
}

void pack_value(Packer &pk, const Value &v, bool as_decimal_string, ActionSerializeStats &st);

// Dict/Obj -> msgpack map in INSERTION order (the composite payloads). EVERY
// key is written, an unfilled one as nil: see omitted() — the composites are
// literal dicts on the Python side, so absence is not a state they can express.
void pack_fields(Packer &pk, const Value::Fields &fs, ActionSerializeStats &st) {
  pk.pack_map(static_cast<std::uint32_t>(fs.size()));
  for (const auto &[key, val] : fs) {
    pack_text(pk, key);
    pack_value(pk, val, decimal_string_key(key), st);
  }
}

void pack_value(Packer &pk, const Value &v, bool as_decimal_string, ActionSerializeStats &st) {
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
    case VType::Float:
      // The natural type, whatever msgpack-c makes of it. Note that
      // packer::pack_double NORMALIZES an integral-valued double to a msgpack
      // INTEGER (pack.hpp:1161) and only emits 0xcb for a fractional one — so
      // 1037.0 goes out as an int and 2.5 as a float64. Both decode: every Go
      // field that can carry a Float is a *string, and decodeFlexString handles
      // the integer codes AND the float codes (rendering the latter with
      // strconv.FormatFloat). The normalization is in fact the safer half — an
      // integral float reaching a numeric field still decodes, which a raw
      // 0xcb would not, since decodeFlexInt has no float branch. No CPython
      // repr(float) reproduction is needed on this side either way.
      st.float_values++;
      pk.pack(v.dnum);
      return;
    case VType::Amount:
      // Value-domain by definition, so always a decimal string regardless of
      // the key. (ActionBuild's av_amount converts Amount -> Int before it ever
      // reaches a fill's output, so this arm is a backstop.)
      if (v.amount_float) {
        st.float_values++;
        pk.pack(v.dnum);
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
      // Should never reach an Action — the BOC-field root-hash substitution is
      // a twin-diff device and must NOT be reused here, so the real bytes go
      // out base64'd and the counter makes the sighting visible.
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
  pk.pack_nil();
}

struct ActionField {
  const char *key;
  Value Action::*member;
};

// The Value-typed Action fields, each omitted when unfilled. SAME SET AND ORDER
// as scrub_arena_refs (EmuClassifierActor.cpp) — two lists that must stay in
// sync; a field added to Action needs a row in both.
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
      {"ton_transfer_data", &Action::ton_transfer_data},
      {"jetton_transfer_data", &Action::jetton_transfer_data},
      {"jetton_swap_data", &Action::jetton_swap_data},
      {"nft_transfer_data", &Action::nft_transfer_data},
      {"nft_listing_data", &Action::nft_listing_data},
      {"nft_mint_data", &Action::nft_mint_data},
      {"dex_deposit_liquidity_data", &Action::dex_deposit_liquidity_data},
      {"dex_withdraw_liquidity_data", &Action::dex_withdraw_liquidity_data},
      {"staking_data", &Action::staking_data},
      {"evaa_supply_data", &Action::evaa_supply_data},
      {"evaa_withdraw_data", &Action::evaa_withdraw_data},
      {"vesting_send_message_data", &Action::vesting_send_message_data},
      {"vesting_add_whitelist_data", &Action::vesting_add_whitelist_data},
      {"tonco_deploy_pool_data", &Action::tonco_deploy_pool_data},
      {"cocoon_worker_payout_data", &Action::cocoon_worker_payout_data},
      {"cocoon_proxy_payout_data", &Action::cocoon_proxy_payout_data},
      {"cocoon_proxy_charge_data", &Action::cocoon_proxy_charge_data},
      {"cocoon_client_top_up_data", &Action::cocoon_client_top_up_data},
      {"cocoon_register_proxy_data", &Action::cocoon_register_proxy_data},
      {"cocoon_unregister_proxy_data", &Action::cocoon_unregister_proxy_data},
      {"cocoon_client_register_data", &Action::cocoon_client_register_data},
      {"cocoon_client_change_secret_hash_data", &Action::cocoon_client_change_secret_hash_data},
      {"cocoon_client_request_refund_data", &Action::cocoon_client_request_refund_data},
      {"cocoon_grant_refund_data", &Action::cocoon_grant_refund_data},
      {"cocoon_client_increase_stake_data", &Action::cocoon_client_increase_stake_data},
      {"cocoon_client_withdraw_data", &Action::cocoon_client_withdraw_data},
      {"layerzero_packet_data", &Action::layerzero_packet_data},
      {"layerzero_send_data", &Action::layerzero_send_data},
      {"layerzero_dvn_verify_data", &Action::layerzero_dvn_verify_data},
      {"jvault_stake_data", &Action::jvault_stake_data},
      {"jvault_claim_data", &Action::jvault_claim_data},
      {"change_dns_record_data", &Action::change_dns_record_data},
      {"coffee_create_pool_data", &Action::coffee_create_pool_data},
      {"coffee_staking_deposit_data", &Action::coffee_staking_deposit_data},
      {"coffee_staking_withdraw_data", &Action::coffee_staking_withdraw_data},
      {"extra", &Action::extra},
  };
  return fields;
}

// Number of always-written keys below (ancestor_type is conditional, so it is
// NOT in this count). Deliberately NOT written: trace_id /
// trace_external_hash / trace_end_lt / trace_end_utime / trace_mc_seqno_end,
// which Go clobbers right after decode (models.go:830-836); trace_start_lt and
// value_extra_currencies, which Python never assigns on this path and Go has no
// field for; and mc_seqno_end, which Go has no field for at all. Omitting them
// also removes eleven chances to get a wire type wrong.
constexpr std::uint32_t kFixedKeys = 12;

void pack_action(Packer &pk, const Action &a, const std::string &trace_key, std::int64_t finality,
                 ActionSerializeStats &st) {
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
  // Not on mch::Action — both come from the view.
  pack_key(pk, "finality");
  pk.pack(finality);
  // Go only surfaces it when it differs from trace_external_hash
  // (models.go:930), so it must be written.
  pack_key(pk, "trace_external_hash_norm");
  pack_text(pk, trace_key);
  // Child-action recursion. Python's asymmetry, reproduced: serialize_blocks
  // ASSIGNS parent_action_id on every row (None on a top-level one), so the key
  // is always in Action.to_dict(); ancestor_type is assigned only when the row
  // is a child, so a top-level row has no such key at all. Go decodes both into
  // zero values either way (emulated/models.go:566-567).
  pack_key(pk, "parent_action_id");
  if (a.parent_action_id.empty()) {
    pk.pack_nil();
  } else {
    pack_text(pk, a.parent_action_id);
  }
  if (!a.ancestor_type.empty()) {
    pack_key(pk, "ancestor_type");
    pk.pack_array(static_cast<std::uint32_t>(a.ancestor_type.size()));
    for (const std::string &t : a.ancestor_type) {
      pack_text(pk, t);
    }
  }

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

std::string serialize_actions(const std::vector<Action> &actions, const EmuTraceView &view,
                              ActionSerializeStats *stats) {
  ActionSerializeStats discarded;
  ActionSerializeStats &st = stats != nullptr ? *stats : discarded;
  // Python's min(tx.finality) over the trace. The emulator's 3-value enum
  // {emulated,confirmed,finalized} is exactly Go's FinalityState
  // {pending,confirmed,finalized}, and an empty view degrades to the
  // conservative end. The derivation lives in EmuTypes.h because the insert
  // guard needs the SAME value — two copies would let the finality a trace is
  // guarded on drift from the one its own payload advertises.
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
