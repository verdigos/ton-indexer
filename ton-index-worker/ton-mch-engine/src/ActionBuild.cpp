#include "ActionBuild.h"

#include "MsgParse.h"

#include "td/utils/base64.h"
#include "td/utils/crypto.h"
#include "td/utils/misc.h"

#include "common/refint.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mch {

bool omitted(const Value &value) {
  return value.t == VType::Null || value.t == VType::Block ||
         ((value.t == VType::Int || value.t == VType::Amount) && value.num.is_null()) ||
         (value.t == VType::Account && value.addr_none) ||
         (value.t == VType::Asset && (value.is_ton || !value.has_jetton)) ||
         (value.t == VType::Cell && value.cell.is_null());
}

namespace {

// Account/Asset/none -> canonical address STRING (Str) or Null.
Value av_addr(const Value &v) {
  if (v.is_null()) return Value::null();
  if (v.t == VType::Asset) return v.has_jetton ? Value::make_str(v.str) : Value::null();
  if (v.t == VType::Account) return v.addr_none ? Value::null() : Value::make_str(v.str);
  return Value::null();
}

// Amount -> its underlying Int or Null; a plain Int (jvault/evaa/tgbtc raw
// amounts) passes through unchanged.
Value av_amount(const Value &v) {
  if (v.is_null()) return Value::null();
  if (v.t == VType::Amount) {
    return v.num.is_null() ? Value::null() : Value::make_int(v.num);
  }
  return v;
}

Value dg(const Value &d, const char *k) {
  const Value *v = d.field(k);
  return v != nullptr ? *v : Value::null();
}
bool flag(const Value &d, const char *k) {
  const Value v = dg(d, k);
  return v.t == VType::Bool && v.boolean;
}

// Lowercase fixed-width 256-bit id, optionally prefixed with "0x".
Value av_hex256(const Value &v, bool prefix) {
  if (v.is_null() || v.t != VType::Int || v.num.is_null()) return Value::null();
  return Value::make_str((prefix ? "0x" : "") + td::hex_string(v.num, false, 64));
}

// uint_hex is general-purpose; only TgBTC's pubkey payload requires 32 bytes.
Value av_pubkey(const Value &v) {
  if (v.t != VType::Str || v.str.empty() || v.str.size() >= 64 ||
      !std::all_of(v.str.begin(), v.str.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      })) return v;
  return Value::make_str(std::string(64 - v.str.size(), '0') + v.str);
}

// jetton/nft `comment`: produced field is RAW bytes (Bytes) or Null. encrypted
// -> base64; else utf-8 backslashreplace + U+0000 strip.
Value comment_value(const Value &d) {
  const Value *c = d.field("comment");
  if (c == nullptr || c->is_null()) return Value::null();
  if (flag(d, "encrypted_comment")) return Value::make_str(td::base64_encode(td::Slice(c->str)));
  return Value::make_str(decode_comment_bytes(c->str));
}

// tick_tock -> tx.now; created_lt set -> created_at; else tx.now.
std::int64_t node_utime(const EventNode *n) {
  if (n->is_tick_tock && n->tx != nullptr) return n->tx->now;
  if (n->msg != nullptr && n->msg->created_lt) {
    return n->msg->created_at ? *n->msg->created_at : 0;
  }
  return n->tx != nullptr ? n->tx->now : 0;
}
Value mkdict(Value::Fields f) {
  std::erase_if(f, [](const auto &field) { return omitted(field.second); });
  return Value::make_dict(std::move(f));
}

void append_fields(Value::Fields &out, const Value &value) {
  if (!value.fields) return;
  out.insert(out.end(), value.fields->begin(), value.fields->end());
}

void append_nft_forward_payout(Value::Fields &out, const Value &d) {
  out.emplace_back("forward_amount", av_amount(dg(d, "forward_amount")));
  out.emplace_back("custom_payload", dg(d, "custom_payload"));
  out.emplace_back("forward_payload", dg(d, "forward_payload"));
  out.emplace_back("response_destination", av_addr(dg(d, "response_destination")));
  out.emplace_back("marketplace_address", av_addr(dg(d, "marketplace_address")));
  out.emplace_back("real_prev_owner", av_addr(dg(d, "real_prev_owner")));
  out.emplace_back("payout_amount", av_amount(dg(d, "payout_amount")));
  out.emplace_back("payout_comment_encrypted", dg(d, "payout_comment_encrypted"));
  out.emplace_back("payout_comment_encoded", dg(d, "payout_comment_encoded"));
  out.emplace_back("payout_comment", dg(d, "payout_comment"));
}

void fill_extra_currencies(const Value &d, Action &a) {
  const Value currencies = dg(d, "extra_currencies");
  if (currencies.t != VType::Dict) return;
  for (const auto &[id, amount] : *currencies.fields) {
    a.value_extra_currencies.emplace(static_cast<std::uint32_t>(std::stoul(id)), amount.str);
  }
}

void append_leg(std::vector<Value> &legs, Value::Fields fields) {
  Value leg = mkdict(std::move(fields));
  legs.push_back(leg.fields && !leg.fields->empty() ? std::move(leg) : Value::null());
}

void append_legs(Value::Fields &fields, std::vector<Value> legs) {
  while (!legs.empty() && legs.back().is_null()) legs.pop_back();
  if (!legs.empty()) fields.emplace_back("legs", Value::make_list(std::move(legs)));
}

Value excesses(const Value &list) {
  std::vector<Value> out;
  if (list.items) {
    for (const Value &item : *list.items) {
      Value asset = Value::null(), amount = Value::null();
      if (item.items && item.items->size() >= 2) {
        asset = (*item.items)[0];
        amount = (*item.items)[1];
      }
      out.push_back(mkdict({{"asset", av_addr(asset)}, {"amount", av_amount(amount)}}));
    }
  }
  return Value::make_list(std::move(out));
}

Value::Fields deposit_leg(const Value &d, int n) {
  const std::string suffix = "_" + std::to_string(n);
  return {{"asset", av_addr(dg(d, ("asset" + suffix).c_str()))},
          {"amount", av_amount(dg(d, ("amount" + suffix).c_str()))},
          {"user_jetton_wallet", av_addr(dg(d, ("user_jetton_wallet" + suffix).c_str()))},
          {"target_asset", av_addr(dg(d, ("target_asset" + suffix).c_str()))},
          {"target_amount", av_amount(dg(d, ("target_amount" + suffix).c_str()))}};
}

Value::Fields withdraw_leg(const Value &d, int n) {
  const std::string suffix = std::to_string(n);
  return {{"amount", av_amount(dg(d, ("amount" + suffix + "_out").c_str()))},
          {"asset", av_addr(dg(d, ("asset" + suffix + "_out").c_str()))},
          {"user_jetton_wallet", av_addr(dg(d, ("wallet" + suffix).c_str()))},
          {"dex_jetton_wallet", av_addr(dg(d, ("dex_jetton_wallet_" + suffix).c_str()))},
          {"dex_wallet", av_addr(dg(d, ("dex_wallet_" + suffix).c_str()))}};
}

std::string fallback_type(const char *type, const Value &d) {
  return std::string(type) + (flag(d, "crippled") ? "_fallback" : "");
}

// Extra participant the generic source/destination assembly cannot derive.
// build_action seeds its account list from here, then dedups, so a duplicate
// push is harmless.
void push_account(Action &a, const Value &v) {
  if (v.t == VType::Str) a.accounts.push_back(v.str);
}

void fill_jetton_transfer(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.source_secondary = av_addr(dg(d, "sender_wallet"));
  a.destination = av_addr(dg(d, "receiver"));
  a.destination_secondary = av_addr(dg(d, "receiver_wallet"));
  a.amount = av_amount(dg(d, "amount"));
  Value asset = dg(d, "asset");
  a.asset = av_addr(asset);
  a.data = mkdict({{"query_id", dg(d, "query_id")},
                   {"response_destination", av_addr(dg(d, "response_address"))},
                   {"forward_amount", av_amount(dg(d, "forward_amount"))},
                   {"custom_payload", dg(d, "custom_payload")},
                   {"forward_payload", dg(d, "forward_payload")},
                   {"comment", comment_value(d)},
                   {"comment_encrypted", dg(d, "encrypted_comment")}});
}

void fill_jetton_burn(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "owner"));
  a.source_secondary = av_addr(dg(d, "jetton_wallet"));
  a.asset = av_addr(dg(d, "asset"));
  a.amount = av_amount(dg(d, "amount"));
}

void fill_jetton_mint(const Value &d, Action &a) {
  a.destination = av_addr(dg(d, "to"));
  a.destination_secondary = av_addr(dg(d, "to_jetton_wallet"));
  a.asset = av_addr(dg(d, "asset"));
  a.amount = av_amount(dg(d, "amount"));
  a.value = av_amount(dg(d, "ton_amount"));
}

Value convert_transfer(const Value &t) {
  return mkdict({{"amount", av_amount(dg(t, "amount"))},
                 {"source", av_addr(dg(t, "source"))},
                 {"source_jetton_wallet", av_addr(dg(t, "source_jetton_wallet"))},
                 {"destination", av_addr(dg(t, "destination"))},
                 {"destination_jetton_wallet", av_addr(dg(t, "destination_jetton_wallet"))},
                 {"asset", av_addr(dg(t, "asset"))}});
}

Value convert_peer_swap(const Value &ps) {
  Value in = dg(ps, "in"), out = dg(ps, "out");
  return mkdict({{"amount_in", av_amount(dg(in, "amount"))},
                 {"asset_in", av_addr(dg(in, "asset"))},
                 {"amount_out", av_amount(dg(out, "amount"))},
                 {"asset_out", av_addr(dg(out, "asset"))}});
}

void fill_jetton_swap(const Value &d, Action &a) {
  Value dit = dg(d, "dex_incoming_transfer");
  Value dot = dg(d, "dex_outgoing_transfer");
  Value in_conv = convert_transfer(dit);
  Value out_conv = convert_transfer(dot);
  a.asset = av_addr(dg(dit, "asset"));
  a.asset2 = av_addr(dg(dot, "asset"));
  Value dex = dg(d, "dex");
  if (dex.t == VType::Str && (dex.str == "stonfi_v2" || dex.str == "dedust" || dex.str == "tonco")) {
    a.asset = av_addr(dg(d, "source_asset"));
    a.asset2 = av_addr(dg(d, "destination_asset"));
  }
  a.source = dg(in_conv, "source");
  a.source_secondary = dg(in_conv, "source_jetton_wallet");
  a.destination = dg(out_conv, "destination");
  a.destination_secondary = dg(out_conv, "destination_jetton_wallet");
  if (!dg(d, "destination_wallet").is_null()) {
    a.destination_secondary = av_addr(dg(d, "destination_wallet"));
  }
  if (!dg(d, "destination_asset").is_null()) {
    a.asset2 = av_addr(dg(d, "destination_asset"));
  }
  Value::Fields f;
  f.emplace_back("protocol", dex);
  f.emplace_back("sender", av_addr(dg(d, "sender")));
  f.emplace_back("dex_incoming_transfer", in_conv);
  f.emplace_back("dex_outgoing_transfer", out_conv);
  f.emplace_back("min_out_amount", av_amount(dg(d, "min_out_amount")));
  if (!dg(d, "peer_swaps").is_null()) {
    Value ps = dg(d, "peer_swaps");
    std::vector<Value> conv;
    if (ps.items) {
      for (const Value &s : *ps.items) conv.push_back(convert_peer_swap(s));
    }
    f.emplace_back("peer_swaps", Value::make_list(std::move(conv)));
  }
  a.data = mkdict(std::move(f));
}

void fill_nft_transfer(const Value &d, Action &a) {
  if (!dg(d, "prev_owner").is_null()) {
    a.source = av_addr(dg(d, "prev_owner"));
  }
  a.destination = av_addr(dg(d, "new_owner"));
  Value nft = dg(d, "nft");
  a.asset_secondary = av_addr(dg(nft, "address"));
  Value coll = dg(nft, "collection");
  if (!coll.is_null()) a.asset = av_addr(dg(coll, "address"));
  bool is_purchase = flag(d, "is_purchase");
  Value::Fields f;
  f.emplace_back("query_id", dg(d, "query_id"));
  f.emplace_back("is_purchase", dg(d, "is_purchase"));
  f.emplace_back("price", is_purchase ? av_amount(dg(d, "price")) : Value::null());
  f.emplace_back("nft_index", dg(nft, "index"));
  append_nft_forward_payout(f, d);
  f.emplace_back("royalty_amount", av_amount(dg(d, "royalty_amount")));
  f.emplace_back("marketplace_fee_address", av_addr(dg(d, "payout_address")));
  f.emplace_back("royalty_address", av_addr(dg(d, "royalty_address")));
  a.data = mkdict(std::move(f));
}

void fill_dedust_deposit_partial(const Value &d, Action &a) {
  a.type = "dex_deposit_liquidity";
  a.source = av_addr(dg(d, "sender"));
  a.destination_secondary = av_addr(dg(d, "deposit_contract"));
  Value::Fields f;
  f.emplace_back("protocol", dg(d, "dex"));
  std::vector<Value> legs;
  append_leg(legs, deposit_leg(d, 1));
  append_leg(legs, deposit_leg(d, 2));
  append_legs(f, std::move(legs));
  a.data = mkdict(std::move(f));
}

void fill_dedust_deposit(const Value &d, Action &a) {
  fill_dedust_deposit_partial(d, a);
  a.destination = av_addr(dg(d, "pool_address"));
  Value::Fields f;
  append_fields(f, a.data);
  f.emplace_back("tokens_minted", av_amount(dg(d, "lp_tokens_minted")));
  f.emplace_back("vault_excesses", excesses(dg(d, "vault_excesses")));
  a.data = mkdict(std::move(f));
}

Value::Fields dedust_v2_claim_leg(const Value &d, const char *suffix) {
  auto field = [&](const char *name) { return dg(d, (std::string(name) + suffix).c_str()); };
  return {{"amount", av_amount(field("amount"))},
          {"asset", av_addr(field("asset"))},
          {"user_jetton_wallet", av_addr(field("user_wallet"))},
          {"dex_wallet", av_addr(field("dex_wallet"))},
          {"dex_jetton_wallet", av_addr(field("dex_jetton_wallet"))}};
}

void fill_dedust_v2_claim(const Value &d, Action &a, const char *first_suffix,
                          const char *second_suffix = nullptr) {
  a.source = av_addr(dg(d, "sender"));
  a.destination = av_addr(dg(d, "pool"));
  a.destination_secondary = av_addr(dg(d, "position"));
  Value::Fields f;
  f.emplace_back("protocol", dg(d, "dex"));
  std::vector<Value> legs;
  append_leg(legs, dedust_v2_claim_leg(d, first_suffix));
  if (second_suffix != nullptr) append_leg(legs, dedust_v2_claim_leg(d, second_suffix));
  append_legs(f, std::move(legs));
  f.emplace_back("is_refund", Value::make_bool(false));
  a.data = mkdict(std::move(f));
}

// Tonco LP shares live in position NFTs rather than LP jettons. Withdrawals
// hard-null source_secondary/asset and always emit is_refund=false.
void fill_tonco_withdraw(const Value &d, Action &a) {
  a.type = "dex_withdraw_liquidity";
  a.source = av_addr(dg(d, "sender"));
  a.source_secondary = Value::null();
  a.destination = av_addr(dg(d, "pool"));
  a.asset = Value::null();
  Value::Fields f;
  f.emplace_back("protocol", Value::make_str("tonco"));
  std::vector<Value> legs;
  append_leg(legs, withdraw_leg(d, 1));
  append_leg(legs, withdraw_leg(d, 2));
  append_legs(f, std::move(legs));
  f.emplace_back("is_refund", Value::make_bool(false));
  f.emplace_back("tokens_burnt", av_amount(dg(d, "liquidity_burnt")));
  f.emplace_back("nft_index", dg(d, "burned_nft_index"));
  f.emplace_back("nft_address", av_addr(dg(d, "burned_nft_address")));
  f.emplace_back("tick_lower", dg(d, "tick_lower"));
  f.emplace_back("tick_upper", dg(d, "tick_upper"));
  a.data = mkdict(std::move(f));
}

void fill_tonco_deposit(const Value &d, Action &a) {
  a.type = "dex_deposit_liquidity";
  a.source = av_addr(dg(d, "sender"));
  Value sw1 = dg(d, "sender_wallet_1"), sw2 = dg(d, "sender_wallet_2");
  a.source_secondary = av_addr(!sw1.is_null() ? sw1 : sw2);
  a.destination = av_addr(dg(d, "pool"));
  a.destination_secondary = av_addr(dg(d, "account_contract"));
  Value amt1 = dg(d, "amount_1"), as1 = dg(d, "asset_1");
  Value amt2 = dg(d, "amount_2"), as2 = dg(d, "asset_2");
  Value::Fields f;
  f.emplace_back("protocol", Value::make_str("tonco"));
  // Tonco legs preserve the pool's slot order.
  std::vector<Value> legs;
  append_leg(legs, {{"amount", av_amount(amt1)},
                         {"asset", av_addr(as1)},
                         {"user_jetton_wallet", av_addr(sw1)},
                         {"target_amount", av_amount(dg(d, "position_amount_1"))},
                         {"target_asset", av_addr(dg(d, "asset_1"))}});
  append_leg(legs, {{"amount", av_amount(amt2)},
                         {"asset", av_addr(as2)},
                         {"user_jetton_wallet", av_addr(sw2)},
                         {"target_amount", av_amount(dg(d, "position_amount_2"))},
                         {"target_asset", av_addr(dg(d, "asset_2"))}});
  append_legs(f, std::move(legs));
  f.emplace_back("tokens_minted", av_amount(dg(d, "lp_tokens_minted")));
  f.emplace_back("tick_lower", dg(d, "tick_lower"));
  f.emplace_back("tick_upper", dg(d, "tick_upper"));
  f.emplace_back("nft_index", dg(d, "nft_index"));
  f.emplace_back("nft_address", av_addr(dg(d, "nft_address")));
  f.emplace_back("vault_excesses", excesses(dg(d, "excesses")));
  a.data = mkdict(std::move(f));
}

// `amount` is a raw Int here (not an Amount), like the withdraw twin.
void fill_evaa_supply(const Value &d, Action &a, bool block_failed) {
  a.source = av_addr(dg(d, "sender"));
  a.source_secondary = av_addr(dg(d, "sender_jetton_wallet"));
  a.destination = av_addr(dg(d, "recipient"));
  a.destination_secondary = av_addr(dg(d, "recipient_contract"));
  a.amount = av_amount(dg(d, "amount"));
  a.asset = av_addr(dg(d, "asset"));
  a.success = flag(d, "is_success");
  if (block_failed) a.success = false;
  a.data = mkdict({{"is_ton", dg(d, "is_ton")},
                   {"asset_id", av_hex256(dg(d, "asset_id"), true)},
                   {"master", av_addr(dg(d, "master"))},
                   {"recipient_jetton_wallet", av_addr(dg(d, "recipient_jetton_wallet"))},
                   {"master_jetton_wallet", av_addr(dg(d, "master_jetton_wallet"))}});
}

void fill_evaa_withdraw(const Value &d, Action &a, bool block_failed) {
  a.source = av_addr(dg(d, "owner"));
  a.destination = av_addr(dg(d, "recipient"));
  a.destination_secondary = av_addr(dg(d, "owner_contract"));
  a.amount = av_amount(dg(d, "amount"));
  a.asset = av_addr(dg(d, "asset"));
  a.success = flag(d, "is_success");
  if (block_failed) a.success = false;
  a.data = mkdict({{"is_ton", dg(d, "is_ton")},
                   {"recipient_jetton_wallet", av_addr(dg(d, "recipient_jetton_wallet"))},
                   {"master_jetton_wallet", av_addr(dg(d, "master_jetton_wallet"))},
                   {"fail_reason", dg(d, "fail_reason")},
                   {"master", av_addr(dg(d, "master"))},
                   {"asset_id", av_hex256(dg(d, "asset_id"), true)}});
}

void fill_evaa_liquidate(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "liquidator"));
  a.destination = av_addr(dg(d, "borrower"));
  a.destination_secondary = av_addr(dg(d, "borrower_contract"));
  a.amount = dg(d, "collateral_amount");
  a.success = flag(d, "is_success");
  a.data = mkdict({{"fail_reason", dg(d, "fail_reason")},
                   {"debt_amount", dg(d, "debt_amount")},
                   {"asset_id", av_hex256(dg(d, "collateral_asset_id"), true)}});
}

void fill_jvault_stake(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.source_secondary = av_addr(dg(d, "sender_wallet"));
  a.asset = av_addr(dg(d, "asset"));
  a.destination = av_addr(dg(d, "staking_pool"));
  a.amount = av_amount(dg(d, "staked_amount"));
  a.data = mkdict({{"period", dg(d, "period")},
                   {"stake_wallet", av_addr(dg(d, "stake_wallet"))}});
}

void fill_jvault_unstake(const Value &d, Action &a, const char *amount_key) {
  a.source = av_addr(dg(d, "sender"));
  a.source_secondary = av_addr(dg(d, "stake_wallet"));
  a.destination = av_addr(dg(d, "staking_pool"));
  a.amount = av_amount(dg(d, amount_key));
  a.asset = av_addr(dg(d, "asset"));
  a.asset2 = av_addr(dg(d, "jvault_asset"));
  a.data = mkdict({{"exit_code", dg(d, "exit_code")}});
}

void fill_jvault_claim(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.source_secondary = av_addr(dg(d, "stake_wallet"));
  a.destination = av_addr(dg(d, "staking_pool"));
  std::vector<Value> rewards;
  const Value jettons = dg(d, "claimed_jettons");
  const Value amounts = dg(d, "claimed_amounts");
  if (jettons.items && amounts.items && jettons.items->size() == amounts.items->size()) {
    for (std::size_t i = 0; i < jettons.items->size(); ++i) {
      rewards.push_back(mkdict({{"asset", av_addr((*jettons.items)[i])},
                                {"amount", av_amount((*amounts.items)[i])}}));
    }
  }
  a.data = mkdict({{"claimed_rewards", Value::make_list(std::move(rewards))}});
}

void fill_ethena_deposit(const Value &d, Action &a) {
  a.type = "stake_deposit";
  a.source = av_addr(dg(d, "source"));
  a.source_secondary = av_addr(dg(d, "user_jetton_wallet"));
  a.destination = av_addr(dg(d, "pool"));
  a.amount = av_amount(dg(d, "value"));
  a.asset = av_addr(dg(d, "asset"));
  a.asset2 = av_addr(dg(d, "source_asset"));
  a.data = mkdict({{"protocol", Value::make_str("ethena")},
                   {"tokens_minted", av_amount(dg(d, "tokens_minted"))}});
}

void fill_layerzero_receive(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.destination = av_addr(dg(d, "oapp"));
  a.destination_secondary = av_addr(dg(d, "channel"));
  Value::Fields f;
  append_fields(f, dg(d, "packet_data"));
  a.data = mkdict(std::move(f));
}

// Populate the LayerZero send fields shared by native and token sends.
// msglib_manager/msglib are payload hex strings, not accounts.
void fill_layerzero_send_fields(const Value &d, Action &a) {
  Value::Fields f;
  f.emplace_back("send_request_id", dg(d, "send_request_id"));
  f.emplace_back("msglib_manager", dg(d, "msglib_manager"));
  f.emplace_back("msglib", dg(d, "msglib"));
  f.emplace_back("uln", av_addr(dg(d, "uln")));
  f.emplace_back("native_fee", dg(d, "native_fee"));
  f.emplace_back("zro_fee", dg(d, "zro_fee"));
  f.emplace_back("endpoint", av_addr(dg(d, "endpoint")));
  f.emplace_back("channel", av_addr(dg(d, "channel")));
  append_fields(f, dg(d, "packet_data"));
  a.data = mkdict(std::move(f));
}

void fill_layerzero_send(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "initiator"));
  fill_layerzero_send_fields(d, a);
}

void fill_layerzero_send_tokens(const Value &d, Action &a) {
  a.source_secondary = av_addr(dg(d, "sender_wallet"));
  a.destination = av_addr(dg(d, "oapp"));
  a.destination_secondary = av_addr(dg(d, "oapp_wallet"));
  a.amount = av_amount(dg(d, "amount"));
  a.asset = av_addr(dg(d, "asset"));
  fill_layerzero_send_fields(dg(d, "layerzero_send_data"), a);
  a.source = av_addr(dg(d, "sender"));
}

// `uln` and `uln_connection` both carry the ULN CONNECTION's address; the
// same local is assigned to both.
void fill_layerzero_commit_packet(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.source_secondary = av_addr(dg(d, "endpoint"));
  a.destination = av_addr(dg(d, "uln"));
  a.destination_secondary = av_addr(dg(d, "uln_connection"));
  Value::Fields f;
  append_fields(f, dg(d, "packet_data"));
  f.emplace_back("channel", av_addr(dg(d, "channel")));
  f.emplace_back("msglib_connection", av_addr(dg(d, "msglib_connection")));
  a.data = mkdict(std::move(f));
}

void fill_layerzero_dvn_verify(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.data = mkdict({{"nonce", dg(d, "nonce")},
                   {"status", dg(d, "status")},
                   {"dvn", av_addr(dg(d, "dvn"))},
                   {"proxy", av_addr(dg(d, "proxy"))},
                   {"uln", av_addr(dg(d, "uln"))},
                   {"uln_connection", av_addr(dg(d, "uln_connection"))}});
}

void fill_coffee_create_vault(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.destination = av_addr(dg(d, "vault"));
  a.asset = av_addr(dg(d, "asset"));
  a.value = av_amount(dg(d, "amount"));
}

// `pool_first` and `pool_second` carry the flat pool-asset pair.

void fill_coffee_create_pool_creator(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.source_secondary = av_addr(dg(d, "sender_jetton_wallet"));
  a.destination = av_addr(dg(d, "deposit_recipient"));
  a.destination_secondary = av_addr(dg(d, "pool_creator_contract"));
  a.asset = av_addr(dg(d, "provided_asset"));
  a.amount = av_amount(dg(d, "amount"));
  a.data = mkdict({{"pool_first", av_addr(dg(d, "pool_first"))},
                   {"pool_second", av_addr(dg(d, "pool_second"))}});
}

void fill_coffee_create_pool(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "source"));
  a.source_secondary = av_addr(dg(d, "source_jetton_wallet"));
  a.amount = av_amount(dg(d, "amount"));
  a.asset = av_addr(dg(d, "asset_1"));
  a.asset2 = av_addr(dg(d, "asset_2"));
  a.destination = av_addr(dg(d, "pool"));
  a.destination_secondary = av_addr(dg(d, "pool_creator_contract"));
  Value::Fields f;
  std::vector<Value> legs;
  append_leg(legs, {{"amount", av_amount(dg(d, "amount_1"))},
                    {"initiator", av_addr(dg(d, "initiator_1"))}});
  append_leg(legs, {{"amount", av_amount(dg(d, "amount_2"))},
                    {"initiator", av_addr(dg(d, "initiator_2"))}});
  append_legs(f, std::move(legs));
  f.emplace_back("provided_asset", av_addr(dg(d, "provided_asset")));
  f.emplace_back("tokens_minted", av_amount(dg(d, "lp_tokens_minted")));
  a.data = mkdict(std::move(f));
}

void fill_coffee_mev_protect_hold_funds(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.source_secondary = av_addr(dg(d, "sender_wallet"));
  a.destination = av_addr(dg(d, "mev_contract"));
  a.destination_secondary = av_addr(dg(d, "mev_contract_wallet"));
  a.asset = av_addr(dg(d, "asset"));
  a.amount = av_amount(dg(d, "amount"));
}

void fill_coffee_mev_protect_failed_swap(const Value &d, Action &a) {
  a.destination = av_addr(dg(d, "recipient"));
  a.asset = av_addr(dg(d, "asset"));
}

void fill_coffee_staking_deposit(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "source"));
  a.source_secondary = av_addr(dg(d, "user_jetton_wallet"));
  a.destination = av_addr(dg(d, "pool"));
  a.destination_secondary = av_addr(dg(d, "pool_jetton_wallet"));
  a.asset = av_addr(dg(d, "asset"));
  a.amount = av_amount(dg(d, "value"));
  // Raw passthrough: the index comes off the nft-item interface record, where
  // it can be a float.
  a.data = mkdict({{"nft_address", av_addr(dg(d, "minted_item_address"))},
                   {"nft_index", dg(d, "minted_item_index")}});
}

void fill_coffee_staking_withdraw(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "source"));
  a.source_secondary = av_addr(dg(d, "user_jetton_wallet"));
  a.destination = av_addr(dg(d, "pool"));
  a.destination_secondary = av_addr(dg(d, "pool_jetton_wallet"));
  a.asset = av_addr(dg(d, "asset"));
  a.amount = av_amount(dg(d, "amount"));
  a.data = mkdict({{"nft_address", av_addr(dg(d, "nft_address"))},
                   {"nft_index", dg(d, "nft_index")},
                   {"points", dg(d, "points")}});
}

void fill_coffee_staking_claim_rewards(const Value &d, Action &a) {
  // `admin` is deliberately dropped (always the same highload wallet, and no
  // directional field fits it); the block still carries it.
  a.source = av_addr(dg(d, "pool"));
  a.source_secondary = av_addr(dg(d, "pool_jetton_wallet"));
  a.destination = av_addr(dg(d, "recipient"));
  a.destination_secondary = av_addr(dg(d, "recipient_jetton_wallet"));
  a.asset = av_addr(dg(d, "asset"));
  a.amount = av_amount(dg(d, "amount"));
}

void fill_vesting_send_message(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.destination = av_addr(dg(d, "vesting"));
  a.destination_secondary = av_addr(dg(d, "message_destination"));
  a.amount = av_amount(dg(d, "message_value"));
  a.success = flag(d, "success");  // the message was actually sent
  a.data = mkdict({{"query_id", dg(d, "query_id")},
                   {"message_boc", dg(d, "message_boc_str")}});
}

// `accounts_added` is a LIST of addresses (the whitelist's unbounded ref chain),
// mapped element-wise through av_addr.
void fill_vesting_add_whitelist(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "adder"));
  a.destination = av_addr(dg(d, "vesting"));
  std::vector<Value> added;
  Value acc = dg(d, "accounts_added");
  if (acc.items) {
    for (const Value &x : *acc.items) {
      Value address = av_addr(x);
      if (!address.is_null()) added.push_back(std::move(address));
    }
  }
  a.data = mkdict({{"query_id", dg(d, "query_id")},
                   {"accounts_added", Value::make_list(std::move(added))}});
}

void fill_multisig_create_order(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "created_by"));
  a.destination = av_addr(dg(d, "multisig"));
  a.destination_secondary = av_addr(dg(d, "order_contract_address"));
  a.data = mkdict({{"query_id", dg(d, "query_id")},
                   {"order_seqno", dg(d, "order_seqno")},
                   {"is_created_by_signer", dg(d, "is_created_by_signer")},
                   {"is_signed_by_creator", dg(d, "creator_approved")},
                   {"signer_index", dg(d, "creator_index")},
                   {"expiration_date", dg(d, "expiration_date")},
                   {"order_boc", dg(d, "order_boc_str")}});
  Value sg = dg(d, "signers");
  if (sg.items) { for (const Value &x : *sg.items) push_account(a, x); }
}

void fill_multisig_approve(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "signer"));
  a.destination = av_addr(dg(d, "order"));
  a.success = flag(d, "success");
  a.data = mkdict({{"signer_index", dg(d, "signer_index")},
                   {"exit_code", dg(d, "exit_code")}});
  Value sg = dg(d, "signers");
  if (sg.items) { for (const Value &x : *sg.items) push_account(a, x); }
}

void fill_multisig_execute(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "order_contract_address"));
  a.destination = av_addr(dg(d, "multisig"));
  a.success = flag(d, "success");
  a.data = mkdict({{"query_id", dg(d, "query_id")},
                   {"order_seqno", dg(d, "order_seqno")},
                   {"expiration_date", dg(d, "expiration_date")},
                   {"approvals_num", dg(d, "approvals_num")},
                   {"signers_hash", dg(d, "signers_hash_str")},
                   {"order_boc", dg(d, "order_boc_str")}});
  Value sg = dg(d, "signers");
  if (sg.items) { for (const Value &x : *sg.items) push_account(a, x); }
}

void fill_tonco_deploy_pool(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "deployer"));
  a.destination = av_addr(dg(d, "router"));
  a.destination_secondary = av_addr(dg(d, "pool"));
  Value::Fields f;
  std::vector<Value> legs;
  append_leg(legs, {{"dex_jetton_wallet", av_addr(dg(d, "jetton0_router_wallet"))},
                    {"asset", av_addr(dg(d, "jetton0_minter"))}});
  append_leg(legs, {{"dex_jetton_wallet", av_addr(dg(d, "jetton1_router_wallet"))},
                    {"asset", av_addr(dg(d, "jetton1_minter"))}});
  append_legs(f, std::move(legs));
  f.emplace_back("tick_spacing", dg(d, "tick_spacing"));
  f.emplace_back("initial_price_x96", dg(d, "initial_price_x96"));
  f.emplace_back("protocol_fee", dg(d, "protocol_fee"));
  f.emplace_back("lp_fee_base", dg(d, "lp_fee_base"));
  f.emplace_back("lp_fee_current", dg(d, "lp_fee_current"));
  f.emplace_back("pool_active", dg(d, "pool_active"));
  a.data = mkdict(std::move(f));
}

// Rewrites the row type like the deposit above: the ethena block btype is
// ethena_withdrawal_request, the ACTION type is the generic
// stake_withdrawal_request with a protocol-tagged payload.
void fill_ethena_withdrawal_request(const Value &d, Action &a) {
  a.type = "stake_withdrawal_request";
  a.source = av_addr(dg(d, "source"));
  a.source_secondary = av_addr(dg(d, "source_wallet"));
  a.destination = av_addr(dg(d, "pool"));
  a.asset = av_addr(dg(d, "asset"));
  a.amount = av_amount(dg(d, "amount"));
  a.data = mkdict({{"protocol", Value::make_str("ethena")},
                   {"tokens_minted", av_amount(dg(d, "ts_usde_amount"))}});
}

void fill_cocoon_worker_payout(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "proxy_contract"));
  a.source_secondary = av_addr(dg(d, "worker_contract"));
  a.destination = av_addr(dg(d, "worker_owner"));
  a.amount = av_amount(dg(d, "payout_amount"));
  a.data = mkdict({{"payout_type", dg(d, "payout_type")},
                   {"query_id", dg(d, "query_id")},
                   {"new_tokens", dg(d, "new_tokens")},
                   {"worker_state", dg(d, "worker_state")},
                   {"worker_tokens", dg(d, "worker_tokens")}});
}

void fill_cocoon_proxy_payout(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "proxy_contract"));
  a.destination = av_addr(dg(d, "proxy_owner"));
  a.destination_secondary = av_addr(dg(d, "excesses_recipient"));
  a.data = mkdict({{"query_id", dg(d, "query_id")}});
}

// `amount = 0` is a LITERAL ("no actual transfer"), not a value read off the
// block.
void fill_cocoon_proxy_charge(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "proxy_contract"));
  a.destination = av_addr(dg(d, "client_contract"));
  a.amount = Value::make_int64(0);
  a.data = mkdict({{"query_id", dg(d, "query_id")},
                   {"new_tokens_used", dg(d, "new_tokens_used")},
                   {"expected_address", dg(d, "expected_address")}});
}

void fill_cocoon_client_top_up(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.destination = av_addr(dg(d, "client_contract"));
  a.destination_secondary = av_addr(dg(d, "proxy_contract"));
  a.amount = av_amount(dg(d, "top_up_amount"));
  a.data = mkdict({{"query_id", dg(d, "query_id")}});
}

void fill_cocoon_proxy_registration(const Value &d, Action &a) {
  a.destination = av_addr(dg(d, "root_contract"));
  a.data = mkdict({{"query_id", dg(d, "query_id")},
                   {"seqno", dg(d, "seqno")}});
}

void fill_cocoon_client_register(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "owner"));
  a.destination = av_addr(dg(d, "client_contract"));
  a.data = mkdict({{"query_id", dg(d, "query_id")},
                   {"nonce", dg(d, "nonce")}});
}

// The payload stores the uint256 as lowercase hex without "0x".
void fill_cocoon_client_change_secret_hash(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "owner"));
  a.destination = av_addr(dg(d, "client_contract"));
  a.data = mkdict({{"query_id", dg(d, "query_id")},
                   {"new_secret_hash", av_hex256(dg(d, "new_secret_hash"), false)}});
}

void fill_cocoon_client_request_refund(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "owner"));
  a.destination = av_addr(dg(d, "client_contract"));
  a.data = mkdict({{"query_id", dg(d, "query_id")},
                   {"via_wallet", dg(d, "via_wallet")}});
}

void fill_cocoon_grant_refund(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "proxy_contract"));
  a.source_secondary = av_addr(dg(d, "client_contract"));
  a.destination = av_addr(dg(d, "refund_recipient"));
  a.amount = av_amount(dg(d, "payout_amount"));
  a.data = mkdict({{"query_id", dg(d, "query_id")},
                   {"new_tokens_used", dg(d, "new_tokens_used")},
                   {"expected_address", dg(d, "expected_address")}});
}

void fill_cocoon_client_amount(const Value &d, Action &a, const char *amount_key) {
  a.source = av_addr(dg(d, "owner"));
  a.destination = av_addr(dg(d, "client_contract"));
  a.amount = av_amount(dg(d, amount_key));
  a.data = mkdict({{"query_id", dg(d, "query_id")}});
}

void fill_tgbtc_mint(const Value &d, Action &a) {
  a.type = fallback_type("tgbtc_mint", d);
  a.source = av_addr(dg(d, "sender"));
  a.destination = av_addr(dg(d, "recipient"));
  a.amount = av_amount(dg(d, "amount"));
  a.asset = av_addr(dg(d, "asset"));
  a.data = mkdict({{"btc_txid", dg(d, "bitcoin_txid")}});
  a.source_secondary = av_addr(dg(d, "teleport_contract"));
  a.destination_secondary = av_addr(dg(d, "recipient_wallet"));
}

// `crippled` (set by tgbtc_burn_log_only) appends the `_fallback` suffix,
// same pattern as the mint.
void fill_tgbtc_burn(const Value &d, Action &a) {
  a.type = fallback_type("tgbtc_burn", d);
  a.source = av_addr(dg(d, "sender"));
  a.source_secondary = av_addr(dg(d, "jetton_wallet"));
  a.destination = av_addr(dg(d, "pegout_address"));
  a.amount = av_amount(dg(d, "amount"));
  a.asset = av_addr(dg(d, "asset"));
}

// `amount` is a raw int (av_amount is the Int passthrough).
void fill_tgbtc_new_key(const Value &d, Action &a) {
  a.type = fallback_type("tgbtc_new_key", d);
  a.source = av_addr(dg(d, "teleport_contract"));
  Value::Fields f{{"pubkey", av_pubkey(dg(d, "pubkey"))}};
  if (a.type != "tgbtc_new_key") f.emplace_back("timestamp", dg(d, "timestamp"));
  a.data = mkdict(std::move(f));
  a.destination = av_addr(dg(d, "coordinator_contract"));
  a.destination_secondary = av_addr(dg(d, "pegout_address"));
  a.amount = av_amount(dg(d, "amount"));
}

// Type is hardcoded `_fallback` (no non-fallback producer, so no `crippled`
// field). Payload key is `pubkey` while the data field is `internal_pubkey`.
void fill_tgbtc_dkg_log(const Value &d, Action &a) {
  a.type = "tgbtc_dkg_log_fallback";
  a.source = av_addr(dg(d, "coordinator_contract"));
  a.data = mkdict({{"pubkey", av_pubkey(dg(d, "internal_pubkey"))},
                   {"timestamp", dg(d, "timestamp")}});
}

void fill_dns_renew(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "source"));
  a.destination = av_addr(dg(d, "destination"));
  a.asset = av_addr(dg(d, "collection_address"));
}

Value bytes_hex(const Value &v) {
  return v.t == VType::Bytes ? Value::make_str(td::hex_encode(v.str)) : Value::null();
}

// DNS records always emit the four database fields. Unknown schemas carry a
// null value and flags.
void fill_change_dns(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "source"));
  a.destination = av_addr(dg(d, "destination"));
  a.asset = av_addr(dg(d, "collection_address"));

  const Value rec = dg(d, "value");
  const Value schema = dg(rec, "schema");
  const std::string s = schema.t == VType::Str ? schema.str : std::string();

  Value value_field = Value::null();
  Value flags = Value::null();
  if (s == "DNSNextResolver" || s == "DNSSmcAddress") {
    value_field = av_addr(dg(rec, "address"));
    if (s == "DNSSmcAddress") {
      flags = dg(rec, "flags");
    }
  } else if (s == "DNSAdnlAddress") {
    value_field = bytes_hex(dg(rec, "address"));
    flags = dg(rec, "flags");
  } else if (s == "DNSStorageAddress") {
    value_field = bytes_hex(dg(rec, "address"));
  } else if (s == "DNSText") {
    value_field = dg(rec, "dns_text");
  }

  a.data = mkdict({{"value_schema", schema},
                   {"flags", flags},
                   {"key", bytes_hex(dg(d, "key"))},
                   {"record_value", value_field}});
}

// Same column with everything but the key nulled. A deletion names the record
// it removes and nothing else.
void fill_delete_dns(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "source"));
  a.destination = av_addr(dg(d, "destination"));
  a.asset = av_addr(dg(d, "collection_address"));
  a.data = mkdict({{"key", bytes_hex(dg(d, "key"))}});
}

void fill_nominator_pool_deposit(const Value &d, Action &a) {
  a.type = "stake_deposit";
  a.source = av_addr(dg(d, "source"));
  a.destination = av_addr(dg(d, "pool"));
  a.amount = av_amount(dg(d, "value"));
  a.data = mkdict({{"protocol", Value::make_str("nominator")}});
}

void fill_nominator_pool_withdraw_request(const Value &d, Action &a) {
  // The btype does NOT decide the action type here: a request that already
  // carries its payout is a COMPLETED withdrawal.
  Value payout = dg(d, "payout_amount");
  if (payout.is_null()) {
    a.type = "stake_withdrawal_request";
  } else {
    a.type = "stake_withdrawal";
    a.amount = av_amount(payout);
  }
  a.data = mkdict({{"protocol", Value::make_str("nominator")}});
  a.source = av_addr(dg(d, "source"));
  a.destination = av_addr(dg(d, "pool"));
}

// Recover omits `amount` when the elector never confirmed.
void fill_election(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "stake_holder"));
  a.amount = av_amount(dg(d, "amount"));
}

void fill_subscribe(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "subscriber"));
  a.destination = av_addr(dg(d, "beneficiary"));
  a.destination_secondary = av_addr(dg(d, "subscription"));
  a.amount = av_amount(dg(d, "amount"));
}

void fill_unsubscribe(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "subscriber"));
  a.destination = av_addr(dg(d, "beneficiary"));
  a.destination_secondary = av_addr(dg(d, "subscription"));
}

void fill_tonstakers_deposit(const Value &d, Action &a) {
  a.type = "stake_deposit";
  a.source = av_addr(dg(d, "source"));
  a.destination = av_addr(dg(d, "pool"));
  a.amount = av_amount(dg(d, "value"));
  a.asset = av_addr(dg(d, "asset"));
  a.data = mkdict({{"protocol", Value::make_str("liquid_staking")},
                   {"tokens_minted", av_amount(dg(d, "tokens_minted"))}});
}

void fill_tonstakers_withdraw(const Value &d, Action &a) {
  a.type = "stake_withdrawal";
  a.source = av_addr(dg(d, "stake_holder"));
  a.destination = av_addr(dg(d, "pool"));
  a.amount = av_amount(dg(d, "amount"));
  a.asset = av_addr(dg(d, "asset"));
  a.data = mkdict({{"protocol", Value::make_str("liquid_staking")},
                   {"nft_address", av_addr(dg(d, "burnt_nft"))},
                   {"tokens_burnt", av_amount(dg(d, "tokens_burnt"))}});
}

void fill_tonstakers_withdraw_request(const Value &d, Action &a) {
  a.type = "stake_withdrawal_request";
  a.source = av_addr(dg(d, "source"));
  a.source_secondary = av_addr(dg(d, "tsTON_wallet"));
  a.destination = av_addr(dg(d, "pool"));
  a.amount = av_amount(dg(d, "tokens_burnt"));
  a.asset = av_addr(dg(d, "asset"));
  a.data = mkdict({{"protocol", Value::make_str("liquid_staking")},
                   {"nft_address", av_addr(dg(d, "minted_nft"))}});
}

// Distinct from the DeDust V1 deposit fills by its input field spellings.
void fill_dex_deposit_liquidity(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.destination = av_addr(dg(d, "pool"));
  Value::Fields f;
  f.emplace_back("protocol", dg(d, "dex"));
  std::vector<Value> legs;
  append_leg(legs, {{"amount", av_amount(dg(d, "amount_1"))},
                         {"asset", av_addr(dg(d, "asset_1"))},
                         {"user_jetton_wallet", av_addr(dg(d, "sender_wallet_1"))}});
  append_leg(legs, {{"amount", av_amount(dg(d, "amount_2"))},
                         {"asset", av_addr(dg(d, "asset_2"))},
                         {"user_jetton_wallet", av_addr(dg(d, "sender_wallet_2"))}});
  append_legs(f, std::move(legs));
  f.emplace_back("tokens_minted", av_amount(dg(d, "lp_tokens_minted")));
  a.data = mkdict(std::move(f));
}

// Not typos vs V1: pool
// lives under `pool` (V1: `pool_address`), user wallets under
// `sender_wallet_N` (V1: `user_jetton_wallet_N`); `target_asset_*` aliases
// asset_1/asset_2 (V2 has no separate target asset, only target amounts).
// `destination_secondary` is written only when the deposit contract is present.
void fill_dedust_v2_deposit(const Value &d, Action &a) {
  a.type = "dex_deposit_liquidity";
  a.source = av_addr(dg(d, "sender"));
  a.destination = av_addr(dg(d, "pool"));
  Value deposit_contract = dg(d, "deposit_contract");
  if (!deposit_contract.is_null()) {
    a.destination_secondary = av_addr(deposit_contract);
  }
  Value::Fields f;
  f.emplace_back("protocol", dg(d, "dex"));
  std::vector<Value> legs;
  append_leg(legs, {{"asset", av_addr(dg(d, "asset_1"))},
                         {"amount", av_amount(dg(d, "amount_1"))},
                         {"user_jetton_wallet", av_addr(dg(d, "sender_wallet_1"))},
                         {"target_asset", av_addr(dg(d, "asset_1"))},
                         {"target_amount", av_amount(dg(d, "target_amount_1"))}});
  append_leg(legs, {{"asset", av_addr(dg(d, "asset_2"))},
                         {"amount", av_amount(dg(d, "amount_2"))},
                         {"user_jetton_wallet", av_addr(dg(d, "sender_wallet_2"))},
                         {"target_asset", av_addr(dg(d, "asset_2"))},
                         {"target_amount", av_amount(dg(d, "target_amount_2"))}});
  append_legs(f, std::move(legs));
  f.emplace_back("tokens_minted", av_amount(dg(d, "lp_tokens_minted")));
  f.emplace_back("vault_excesses", excesses(dg(d, "vault_excesses")));
  a.data = mkdict(std::move(f));
}

// A partial has no target amounts or vault excesses.
void fill_dedust_v2_deposit_partial(const Value &d, Action &a) {
  a.type = "dex_deposit_liquidity";
  fill_dex_deposit_liquidity(d, a);
  Value deposit_contract = dg(d, "deposit_contract");
  if (!deposit_contract.is_null()) {
    a.destination_secondary = av_addr(deposit_contract);
  }
}

void fill_dex_withdraw_liquidity(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.source_secondary = av_addr(dg(d, "sender_wallet"));
  a.destination = av_addr(dg(d, "pool"));
  a.asset = av_addr(dg(d, "asset"));
  Value::Fields f;
  f.emplace_back("protocol", dg(d, "dex"));
  std::vector<Value> legs;
  append_leg(legs, withdraw_leg(d, 1));
  append_leg(legs, withdraw_leg(d, 2));
  append_legs(f, std::move(legs));
  f.emplace_back("is_refund", dg(d, "is_refund"));
  f.emplace_back("tokens_burnt", av_amount(dg(d, "lp_tokens_burnt")));
  a.data = mkdict(std::move(f));
}

// getgems / telegram NFT purchase.
void fill_nft_purchase(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "prev_owner"));
  a.destination = av_addr(dg(d, "new_owner"));
  a.asset_secondary = av_addr(dg(d, "nft_address"));
  a.asset = av_addr(dg(d, "collection_address"));
  Value::Fields f;
  f.emplace_back("query_id", dg(d, "query_id"));
  // The DNS purchase API has no is_purchase field.
  if (a.type != "dns_purchase") f.emplace_back("is_purchase", Value::make_bool(true));
  f.emplace_back("price", av_amount(dg(d, "price")));
  f.emplace_back("nft_index", dg(d, "nft_index"));
  append_nft_forward_payout(f, d);
  a.data = mkdict(std::move(f));
}

void fill_dns_release(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "source"));
  a.destination = av_addr(dg(d, "nft_address"));
  a.asset = av_addr(dg(d, "nft_collection"));
  a.data = mkdict({{"query_id", dg(d, "query_id")},
                   {"nft_index", dg(d, "nft_index")}});
  a.value = av_amount(dg(d, "value"));
}

void fill_nft_put_on_auction(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "owner"));
  a.source_secondary = av_addr(dg(d, "listing_address"));
  a.destination = av_addr(dg(d, "auction_address"));
  a.asset = av_addr(dg(d, "nft_collection"));
  a.asset_secondary = av_addr(dg(d, "nft_address"));
  a.data = mkdict({{"marketplace_address", av_addr(dg(d, "marketplace_address"))},
                   {"nft_index", dg(d, "nft_index")},
                   {"mp_fee_factor", av_amount(dg(d, "mp_fee_factor"))},
                   {"mp_fee_base", av_amount(dg(d, "mp_fee_base"))},
                   {"royalty_fee_base", av_amount(dg(d, "royalty_fee_base"))},
                   {"max_bid", av_amount(dg(d, "max_bid"))},
                   {"min_bid", av_amount(dg(d, "min_bid"))},
                   {"marketplace_fee_address", av_addr(dg(d, "mp_fee_address"))},
                   {"royalty_address", av_addr(dg(d, "royalty_fee_addr"))}});
}

// Shared by teleitem_cancel_auction and the three GetGems cancel/finish
// btypes. For GetGems the destination is the sale/auction contract and
// asset_secondary is the NFT — an account generic assembly cannot reach,
// because those matchers do not consume the nft_transfer that would have
// carried it. push_account + dedup covers both cases.
void fill_cancel_nft_trade(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "owner"));
  a.destination = av_addr(dg(d, "trade_contract"));
  a.asset_secondary = av_addr(dg(d, "nft_address"));
  a.asset = av_addr(dg(d, "nft_collection"));
  a.data = mkdict({{"marketplace_address", av_addr(dg(d, "marketplace_address"))}});
  push_account(a, a.asset_secondary);
}

void fill_nft_put_on_sale(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "owner"));
  a.source_secondary = av_addr(dg(d, "listing_address"));
  a.destination = av_addr(dg(d, "sale_address"));
  a.asset = av_addr(dg(d, "nft_collection"));
  a.asset_secondary = av_addr(dg(d, "nft_address"));
  a.data = mkdict({{"marketplace_address", av_addr(dg(d, "marketplace_address"))},
                   {"nft_index", dg(d, "nft_index")},
                   {"full_price", av_amount(dg(d, "full_price"))},
                   {"marketplace_fee", av_amount(dg(d, "marketplace_fee"))},
                   {"royalty_amount", av_amount(dg(d, "royalty_amount"))},
                   {"marketplace_fee_address", av_addr(dg(d, "marketplace_fee_address"))},
                   {"royalty_address", av_addr(dg(d, "royalty_address"))}});
}

// asset_secondary here is the NFT (neither source nor destination), so it
// needs the extra push.
void fill_sale_update(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.destination = av_addr(dg(d, "sale_contract"));
  a.asset_secondary = av_addr(dg(d, "nft_address"));
  a.data = mkdict({{"marketplace_address", av_addr(dg(d, "marketplace_address"))},
                   {"full_price", av_amount(dg(d, "new_full_price"))},
                   {"marketplace_fee", av_amount(dg(d, "new_marketplace_fee"))},
                   {"royalty_amount", av_amount(dg(d, "new_royalty_amount"))}});
  push_account(a, a.asset_secondary);
}

void fill_auction_bid(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "bidder"));
  a.destination = av_addr(dg(d, "auction"));
  a.asset_secondary = av_addr(dg(d, "nft_address"));
  a.asset = av_addr(dg(d, "nft_collection"));
  a.data = mkdict({{"nft_index", dg(d, "nft_item_index")}});
  push_account(a, a.asset_secondary);
  a.value = dg(d, "amount");  // raw int, not av_amount
}

void fill_auction_outbid(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "auction_address"));
  a.destination = av_addr(dg(d, "bidder"));
  a.source_secondary = av_addr(dg(d, "new_bidder"));
  a.asset_secondary = av_addr(dg(d, "nft"));
  a.asset = av_addr(dg(d, "nft_collection"));
  a.amount = av_amount(dg(d, "amount"));
  a.data = mkdict({{"comment", dg(d, "comment")}});
  push_account(a, a.asset_secondary);
}

// `destination` is deliberately never set.
void fill_nft_discovery(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.asset = av_addr(dg(d, "result_collection"));
  a.asset_secondary = av_addr(dg(d, "nft"));
  a.data = mkdict({{"nft_index", dg(d, "result_index")}});
}

// `opcode` and `nft_index` are raw passthroughs; the index is already an Int.
void fill_nft_mint(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "source"));
  a.destination = av_addr(dg(d, "address"));
  a.asset_secondary = a.destination;
  a.opcode = dg(d, "opcode");
  a.asset = av_addr(dg(d, "collection"));
  a.data = mkdict({{"nft_index", dg(d, "index")}});
}

void fill_call_contract(const Value &d, Action &a) {
  a.opcode = dg(d, "opcode");
  a.value = av_amount(dg(d, "value"));
  a.source = av_addr(dg(d, "source"));
  a.destination = av_addr(dg(d, "destination"));
  fill_extra_currencies(d, a);
}

void fill_ton_transfer(const Value &d, Action &a) {
  a.value = av_amount(dg(d, "value"));
  a.source = av_addr(dg(d, "source"));
  a.destination = av_addr(dg(d, "destination"));
  fill_extra_currencies(d, a);
  a.data = mkdict({{"comment", dg(d, "comment")},  // leaf_comment_info already strips U+0000
                   {"comment_encrypted", dg(d, "encrypted")}});
}

void fill_tick_tock(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "account"));
}

// Returns false for a btype outside the fill set (skip-table entry).
bool fill_action(Block *b, Action &a) {
  const Value &d = b->data;
  const std::string &t = b->btype;
  if (t == "call_contract" || t == "contract_deploy" || t == "gasless_request" ||
      t == "change_wallet_key") {
    fill_call_contract(d, a);  // the two wallet types carry only source/destination/value
  }
  else if (t == "ton_transfer") fill_ton_transfer(d, a);
  else if (t == "tick_tock") fill_tick_tock(d, a);
  else if (t == "jetton_transfer") fill_jetton_transfer(d, a);
  else if (t == "jetton_burn") fill_jetton_burn(d, a);
  else if (t == "jetton_mint") fill_jetton_mint(d, a);
  else if (t == "jetton_swap") fill_jetton_swap(d, a);
  else if (t == "nft_transfer") fill_nft_transfer(d, a);
  else if (t == "dedust_deposit_liquidity") fill_dedust_deposit(d, a);
  // Coffee uses the same fields as the DeDust deposit.
  else if (t == "coffee_deposit_liquidity") fill_dedust_deposit(d, a);
  else if (t == "dedust_deposit_liquidity_partial") fill_dedust_deposit_partial(d, a);
  else if (t == "dedust_v2_deposit_liquidity") fill_dedust_v2_deposit(d, a);
  else if (t == "dedust_v2_deposit_liquidity_partial") fill_dedust_v2_deposit_partial(d, a);
  else if (t == "tonco_deposit_liquidity") fill_tonco_deposit(d, a);
  else if (t == "tonco_withdraw_liquidity") fill_tonco_withdraw(d, a);
  else if (t == "dedust_v2_claim_fees") fill_dedust_v2_claim(d, a, "_x", "_y");
  else if (t == "dedust_v2_claim_reward") fill_dedust_v2_claim(d, a, "");
  else if (t == "evaa_supply") fill_evaa_supply(d, a, b->failed);
  else if (t == "evaa_withdraw") fill_evaa_withdraw(d, a, b->failed);
  else if (t == "evaa_liquidate") fill_evaa_liquidate(d, a);
  else if (t == "jvault_stake") fill_jvault_stake(d, a);
  else if (t == "jvault_unstake") fill_jvault_unstake(d, a, "unstaked_amount");
  else if (t == "jvault_unstake_request") fill_jvault_unstake(d, a, "requested_amount");
  else if (t == "jvault_claim") fill_jvault_claim(d, a);
  else if (t == "ethena_deposit") fill_ethena_deposit(d, a);
  else if (t == "layerzero_receive") fill_layerzero_receive(d, a);
  else if (t == "layerzero_send") fill_layerzero_send(d, a);
  else if (t == "layerzero_send_tokens") fill_layerzero_send_tokens(d, a);
  else if (t == "layerzero_commit_packet") fill_layerzero_commit_packet(d, a);
  else if (t == "layerzero_dvn_verify") fill_layerzero_dvn_verify(d, a);
  else if (t == "coffee_create_vault") fill_coffee_create_vault(d, a);
  else if (t == "coffee_create_pool_creator") fill_coffee_create_pool_creator(d, a);
  else if (t == "coffee_create_pool") fill_coffee_create_pool(d, a);
  else if (t == "coffee_mev_protect_hold_funds") fill_coffee_mev_protect_hold_funds(d, a);
  else if (t == "coffee_mev_protect_failed_swap") fill_coffee_mev_protect_failed_swap(d, a);
  else if (t == "coffee_staking_deposit") fill_coffee_staking_deposit(d, a);
  else if (t == "coffee_staking_withdraw") fill_coffee_staking_withdraw(d, a);
  else if (t == "coffee_staking_claim_rewards") fill_coffee_staking_claim_rewards(d, a);
  else if (t == "vesting_send_message") fill_vesting_send_message(d, a);
  else if (t == "vesting_add_whitelist") fill_vesting_add_whitelist(d, a);
  else if (t == "multisig_create_order") fill_multisig_create_order(d, a);
  else if (t == "multisig_approve") fill_multisig_approve(d, a);
  else if (t == "multisig_execute") fill_multisig_execute(d, a);
  else if (t == "tonco_deploy_pool") fill_tonco_deploy_pool(d, a);
  else if (t == "ethena_withdrawal_request") fill_ethena_withdrawal_request(d, a);
  else if (t == "cocoon_worker_payout") fill_cocoon_worker_payout(d, a);
  else if (t == "cocoon_proxy_payout") fill_cocoon_proxy_payout(d, a);
  else if (t == "cocoon_proxy_charge") fill_cocoon_proxy_charge(d, a);
  else if (t == "cocoon_client_top_up") fill_cocoon_client_top_up(d, a);
  else if (t == "cocoon_register_proxy" || t == "cocoon_unregister_proxy")
    fill_cocoon_proxy_registration(d, a);
  else if (t == "cocoon_client_register") fill_cocoon_client_register(d, a);
  else if (t == "cocoon_client_change_secret_hash") fill_cocoon_client_change_secret_hash(d, a);
  else if (t == "cocoon_client_request_refund") fill_cocoon_client_request_refund(d, a);
  else if (t == "cocoon_grant_refund") fill_cocoon_grant_refund(d, a);
  else if (t == "cocoon_client_increase_stake") fill_cocoon_client_amount(d, a, "new_stake");
  else if (t == "cocoon_client_withdraw") fill_cocoon_client_amount(d, a, "withdraw_amount");
  else if (t == "tgbtc_mint") fill_tgbtc_mint(d, a);
  else if (t == "tgbtc_burn") fill_tgbtc_burn(d, a);
  else if (t == "tgbtc_new_key") fill_tgbtc_new_key(d, a);
  else if (t == "tgbtc_dkg_log") fill_tgbtc_dkg_log(d, a);
  else if (t == "renew_dns") fill_dns_renew(d, a);
  else if (t == "nominator_pool_deposit") fill_nominator_pool_deposit(d, a);
  else if (t == "nominator_pool_withdraw_request") fill_nominator_pool_withdraw_request(d, a);
  else if (t == "election_deposit" || t == "election_recover") fill_election(d, a);
  else if (t == "subscribe") fill_subscribe(d, a);
  else if (t == "unsubscribe") fill_unsubscribe(d, a);
  else if (t == "tonstakers_deposit") fill_tonstakers_deposit(d, a);
  else if (t == "tonstakers_withdraw") fill_tonstakers_withdraw(d, a);
  else if (t == "tonstakers_withdraw_request") fill_tonstakers_withdraw_request(d, a);
  else if (t == "dex_deposit_liquidity") fill_dex_deposit_liquidity(d, a);
  else if (t == "dex_withdraw_liquidity") fill_dex_withdraw_liquidity(d, a);
  else if (t == "nft_purchase") fill_nft_purchase(d, a);
  else if (t == "dns_purchase") fill_nft_purchase(d, a);
  else if (t == "dns_release") fill_dns_release(d, a);
  else if (t == "change_dns") fill_change_dns(d, a);
  else if (t == "delete_dns") fill_delete_dns(d, a);
  else if (t == "teleitem_start_auction" || t == "nft_put_on_auction")
    fill_nft_put_on_auction(d, a);
  else if (t == "teleitem_cancel_auction" || t == "nft_cancel_sale" ||
           t == "nft_cancel_auction" || t == "nft_finish_auction")
    fill_cancel_nft_trade(d, a);
  else if (t == "nft_put_on_sale") fill_nft_put_on_sale(d, a);
  else if (t == "nft_update_sale") fill_sale_update(d, a);
  else if (t == "auction_bid") fill_auction_bid(d, a);
  else if (t == "auction_outbid") fill_auction_outbid(d, a);
  else if (t == "nft_discovery") fill_nft_discovery(d, a);
  else if (t == "nft_mint") fill_nft_mint(d, a);
  else return false;
  return true;
}
}  // namespace

const EventNode *root_event_node(const Block *b) {
  const EventNode *root = nullptr;
  for (const EventNode *n : b->event_nodes) {
    if (root == nullptr || n->lt() < root->lt()) root = n;
  }
  return root;
}

std::string calc_action_id(const Block *b) {
  const EventNode *root = root_event_node(b);
  std::string key;
  if (root != nullptr) {
    key = (root->msg != nullptr) ? root->msg->msg_hash : root->tx_hash();
  }
  key += b->btype;
  return td::base64_encode(td::Slice(td::sha256(td::Slice(key))));
}

static bool build_action_with_id(Block *b, const std::string &action_id, Action &a) {
  a.type = b->btype;
  a.success = !b->failed;
  a.action_id = action_id;
  std::set<std::string> ths;
  const EventNode *root_tx_node = nullptr;
  std::int64_t maxlt = std::numeric_limits<std::int64_t>::min();
  std::int64_t minu = std::numeric_limits<std::int64_t>::max();
  std::int64_t maxu = std::numeric_limits<std::int64_t>::min();
  std::int64_t seqmax = std::numeric_limits<std::int64_t>::min();
  for (const EventNode *n : b->event_nodes) {
    ths.insert(n->tx_hash());
    if (n->tx != nullptr &&
        (root_tx_node == nullptr || n->tx->lt < root_tx_node->tx->lt ||
         (n->tx->lt == root_tx_node->tx->lt && n->tx->hash < root_tx_node->tx->hash))) {
      root_tx_node = n;
    }
    std::int64_t tlt = n->tx != nullptr ? n->tx->lt : 0;
    maxlt = std::max(maxlt, tlt);
    minu = std::min(minu, node_utime(n));
    maxu = std::max(maxu, n->tx != nullptr ? n->tx->now : 0);
    if (n->tx != nullptr) seqmax = std::max(seqmax, n->tx->mc_block_seqno);
  }
  if (b->initiating_event_node != nullptr) {
    ths.insert(b->initiating_event_node->tx_hash());
  }
  // The transaction that started the action comes first; the rest stay sorted.
  a.tx_hashes.clear();
  const EventNode *first_node = b->initiating_event_node != nullptr ? b->initiating_event_node : root_tx_node;
  if (first_node != nullptr) {
    const auto first_hash = first_node->tx_hash();
    a.tx_hashes.push_back(first_hash);
    ths.erase(first_hash);
  }
  a.tx_hashes.insert(a.tx_hashes.end(), ths.begin(), ths.end());
  a.start_lt = b->min_lt;
  a.end_lt = b->event_nodes.empty() ? 0 : maxlt;
  a.start_utime = b->event_nodes.empty() ? 0 : minu;
  a.end_utime = b->event_nodes.empty() ? 0 : maxu;
  a.mc_seqno_end = b->event_nodes.empty() ? 0 : seqmax;
  if (!fill_action(b, a)) return false;

  const std::pair<const char *, const Value *> numeric_fields[]{
      {"amount", &a.amount}, {"value", &a.value}};
  for (const auto &[name, value] : numeric_fields) {
    if (value->t != VType::Str && value->t != VType::Int && value->t != VType::Amount &&
        value->t != VType::Null) {
      throw std::runtime_error("action build produced invalid " + std::string(name) +
                               " for " + b->btype);
    }
  }
  if (a.opcode.t != VType::Int && a.opcode.t != VType::Null) {
    throw std::runtime_error("action build produced invalid opcode for " + b->btype);
  }

// Role-less accounts: each event_node's tx account; + source/source_secondary;
// + destination/destination_secondary unless ghost; + the initiating tx
// account (non-tick_tock). Dedup, drop nulls. Seeded from whatever the fill
// already pushed (NFT sale/auction asset_secondary and similar extras).
  std::vector<std::string> accts = std::move(a.accounts);
  a.accounts.clear();
  for (const EventNode *n : b->event_nodes) {
    if (n->tx != nullptr) accts.push_back(n->tx->account);
  }
  auto push_acc = [&](const Value &v) {
    if (v.t == VType::Str) accts.push_back(v.str);
  };
  push_acc(a.source);
  push_acc(a.source_secondary);
  if (!b->is_ghost_block) {
    push_acc(a.destination);
    push_acc(a.destination_secondary);
  }
  if (b->initiating_event_node != nullptr && !b->initiating_event_node->is_tick_tock &&
      b->initiating_event_node->tx != nullptr) {
    accts.push_back(b->initiating_event_node->tx->account);
  }
  std::sort(accts.begin(), accts.end());
  accts.erase(std::unique(accts.begin(), accts.end()), accts.end());
  a.accounts = std::move(accts);
  return true;
}

bool build_action(Block *b, Action &a) {
  return build_action_with_id(b, calc_action_id(b), a);
}

bool build_action(const ActionRow &row, Action &a) {
  if (!build_action_with_id(row.block, row.action_id, a)) return false;
  a.parent_action_id = row.parent_action_id;
  a.ancestor_type = row.ancestor_type;
  if (!row.parent_gasless_action.empty()) {
    Value::Fields f = a.data.fields ? *a.data.fields : Value::Fields{};
    f.emplace_back("parent_gasless_action", Value::make_str(row.parent_gasless_action));
    a.data = mkdict(std::move(f));
  }
  return true;
}

Action create_unknown_action(const Trace &trace) {
  Action a;
  a.type = "unknown";
  a.action_id = trace.trace_id;
  // Every loader must populate these trace aggregates.
  a.start_lt = trace.start_lt;
  a.end_lt = trace.end_lt;
  a.start_utime = trace.start_utime;
  a.end_utime = trace.end_utime;
  a.mc_seqno_end = trace.mc_seqno_end;
  // success = not any(tx.aborted): a property of the whole trace, not a block.
  bool failed = false;
  std::set<std::string> accts;
  for (const auto &tx : trace.transactions) {
    a.tx_hashes.push_back(tx->hash);
    accts.insert(tx->account);
    failed = failed || tx->aborted;
  }
  a.success = !failed;
  // Sorted for determinism; consumers and A/B comparators sort anyway.
  std::sort(a.tx_hashes.begin(), a.tx_hashes.end());
  a.tx_hashes.erase(std::unique(a.tx_hashes.begin(), a.tx_hashes.end()), a.tx_hashes.end());
  a.accounts.assign(accts.begin(), accts.end());
  return a;
}

}  // namespace mch
