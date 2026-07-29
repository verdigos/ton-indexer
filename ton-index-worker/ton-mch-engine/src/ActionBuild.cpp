#include "ActionBuild.h"

#include "MsgParse.h"

#include "td/utils/base64.h"
#include "td/utils/crypto.h"

#include "common/refint.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace mch {

namespace {

// ===========================================================================
// Phase E part 2: C++ Action production (block_tree_serializer.py port).
//
// Scope: the composite btypes that FIRE in the corpus classify dump (19 of
// them), rendered per surviving post-pass PendingAction — the same block set
// the classify dump emits. The serializer's spine-walk / child-recursion
// wrapper (parent_action_id, ancestor_type, v1_ops) is OUT of scope; the basic
// leaf fills (call_contract/ton_transfer/contract_deploy) exist but are reached
// only on the fallback path, where the C++ leaf .data is a subset (§10 hazard).
//
// NOT rendered: roles (this branch's action_accounts are role-less, prod-design
// §6.1), trace_id / trace_* / finality (constant per fixture). accounts AND the
// block_to_action tx_hashes extension by initiating_event_node ARE reproduced
// (Step 2a) — Block carries initiating_event_node.
// ===========================================================================

// _addr(): AccountId/Asset/None -> canonical address STRING (Str) or Null.
// Composites and directional fields store addresses as plain strings (Python
// _addr / .as_str()), so they render with the "str:" prefix on both sides.
Value av_addr(const Value &v) {
  if (v.is_null()) return Value::null();
  if (v.t == VType::Asset) return v.has_jetton ? Value::make_str(v.str) : Value::null();
  if (v.t == VType::Account) return v.addr_none ? Value::null() : Value::make_str(v.str);
  return Value::null();
}

// Amount.value / _value(): Amount -> its underlying Int (or Float / Null);
// a plain Int (jvault/evaa/tgbtc raw amounts) passes through unchanged.
Value av_amount(const Value &v) {
  if (v.is_null()) return Value::null();
  if (v.t == VType::Amount) {
    if (v.amount_float) return Value::make_float(v.dnum);
    return v.num.is_null() ? Value::null() : Value::make_int(v.num);
  }
  return v;  // Int / Float passthrough
}

// data field read (Dict/Obj); missing == Null.
Value dg(const Value &d, const char *k) {
  const Value *v = d.field(k);
  return v != nullptr ? *v : Value::null();
}
bool has(const Value &d, const char *k) { return d.field(k) != nullptr; }

// Python hex(int): "0x" + lowercase magnitude, no leading zeros. Only used for
// EVAA asset_id (a positive 256-bit id). Null -> Null.
Value av_hex(const Value &v) {
  if (v.is_null() || v.t != VType::Int || v.num.is_null()) return Value::null();
  return Value::make_str("0x" + td::hex_string(v.num, false));
}

// jetton/nft `comment`: produced field is RAW bytes (Bytes) or Null. encrypted
// -> base64; else utf-8 backslashreplace + U+0000 strip (decode_comment_bytes).
Value comment_value(const Value &d) {
  const Value *c = d.field("comment");
  if (c == nullptr || c->is_null()) return Value::null();
  bool enc = false;
  const Value *e = d.field("encrypted_comment");
  if (e != nullptr && e->t == VType::Bool) enc = e->boolean;
  if (enc) return Value::make_str(td::base64_encode(td::Slice(c->str)));
  return Value::make_str(decode_comment_bytes(c->str));
}

// get_utime (tree_utils.py): tick_tock -> tx.now; created_lt set -> created_at;
// else tx.now.
std::int64_t node_utime(const EventNode *n) {
  if (n->is_tick_tock && n->tx != nullptr) return n->tx->now;
  if (n->msg != nullptr && n->msg->created_lt) {
    return n->msg->created_at ? *n->msg->created_at : 0;
  }
  return n->tx != nullptr ? n->tx->now : 0;
}
Value mkdict(Value::Fields f) { return Value::make_dict(std::move(f)); }

// --- per-btype fills (block_tree_serializer.py _fill_*) ---------------------

void fill_jetton_transfer(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.source_secondary = av_addr(dg(d, "sender_wallet"));
  a.destination = av_addr(dg(d, "receiver"));
  a.destination_secondary = has(d, "receiver_wallet") ? av_addr(dg(d, "receiver_wallet")) : Value::null();
  a.amount = av_amount(dg(d, "amount"));
  Value asset = dg(d, "asset");
  a.asset = (asset.is_null() || (asset.t == VType::Asset && asset.is_ton)) ? Value::null() : av_addr(asset);
  Value::Fields f;
  f.emplace_back("query_id", dg(d, "query_id"));
  f.emplace_back("response_destination", av_addr(dg(d, "response_address")));
  f.emplace_back("forward_amount", av_amount(dg(d, "forward_amount")));
  f.emplace_back("custom_payload", dg(d, "custom_payload"));
  f.emplace_back("forward_payload", dg(d, "forward_payload"));
  f.emplace_back("comment", comment_value(d));
  f.emplace_back("is_encrypted_comment", dg(d, "encrypted_comment"));
  a.jetton_transfer_data = mkdict(std::move(f));
}

void fill_jetton_burn(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "owner"));
  a.source_secondary = av_addr(dg(d, "jetton_wallet"));
  a.asset = av_addr(dg(d, "asset"));  // asset.jetton_address.as_str()
  a.amount = av_amount(dg(d, "amount"));
}

void fill_jetton_mint(const Value &d, Action &a) {
  a.destination = av_addr(dg(d, "to"));
  a.destination_secondary = av_addr(dg(d, "to_jetton_wallet"));
  a.asset = av_addr(dg(d, "asset"));  // asset.jetton_address
  a.amount = av_amount(dg(d, "amount"));
  a.value = av_amount(dg(d, "ton_amount"));
}

Value convert_transfer(const Value &t) {
  Value::Fields f;
  f.emplace_back("amount", av_amount(dg(t, "amount")));
  f.emplace_back("source", av_addr(dg(t, "source")));
  f.emplace_back("source_jetton_wallet", av_addr(dg(t, "source_jetton_wallet")));
  f.emplace_back("destination", av_addr(dg(t, "destination")));
  f.emplace_back("destination_jetton_wallet", av_addr(dg(t, "destination_jetton_wallet")));
  f.emplace_back("asset", av_addr(dg(t, "asset")));
  return mkdict(std::move(f));
}

Value convert_peer_swap(const Value &ps) {
  Value in = dg(ps, "in"), out = dg(ps, "out");
  Value::Fields f;
  f.emplace_back("amount_in", av_amount(dg(in, "amount")));
  f.emplace_back("asset_in", av_addr(dg(in, "asset")));
  f.emplace_back("amount_out", av_amount(dg(out, "amount")));
  f.emplace_back("asset_out", av_addr(dg(out, "asset")));
  return mkdict(std::move(f));
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
  if (has(d, "destination_wallet") && !dg(d, "destination_wallet").is_null()) {
    a.destination_secondary = av_addr(dg(d, "destination_wallet"));
  }
  if (has(d, "destination_asset") && !dg(d, "destination_asset").is_null()) {
    a.asset2 = av_addr(dg(d, "destination_asset"));
  }
  Value min_out = Value::null();
  if (has(d, "min_out_amount")) min_out = av_amount(dg(d, "min_out_amount"));
  Value::Fields f;
  f.emplace_back("dex", dex);
  f.emplace_back("sender", av_addr(dg(d, "sender")));
  f.emplace_back("dex_incoming_transfer", in_conv);
  f.emplace_back("dex_outgoing_transfer", out_conv);
  f.emplace_back("min_out_amount", min_out);
  if (has(d, "peer_swaps") && !dg(d, "peer_swaps").is_null()) {
    Value ps = dg(d, "peer_swaps");
    std::vector<Value> conv;
    if (ps.items) {
      for (const Value &s : *ps.items) conv.push_back(convert_peer_swap(s));
    }
    f.emplace_back("peer_swaps", Value::make_list(std::move(conv)));
  }
  a.jetton_swap_data = mkdict(std::move(f));
}

void fill_nft_transfer(const Value &d, Action &a) {
  if (has(d, "prev_owner") && !dg(d, "prev_owner").is_null()) {
    a.source = av_addr(dg(d, "prev_owner"));
  }
  a.destination = av_addr(dg(d, "new_owner"));
  Value nft = dg(d, "nft");
  a.asset_secondary = av_addr(dg(nft, "address"));
  Value coll = dg(nft, "collection");
  if (!coll.is_null()) a.asset = av_addr(dg(coll, "address"));
  bool is_purchase = dg(d, "is_purchase").t == VType::Bool && dg(d, "is_purchase").boolean;
  Value::Fields f;
  f.emplace_back("query_id", dg(d, "query_id"));
  f.emplace_back("is_purchase", dg(d, "is_purchase"));
  f.emplace_back("price", (has(d, "price") && is_purchase) ? av_amount(dg(d, "price")) : Value::null());
  f.emplace_back("nft_item_index", dg(nft, "index"));  // Float passthrough
  Value fwd = dg(d, "forward_amount");
  f.emplace_back("forward_amount", fwd.is_null() ? Value::null() : av_amount(fwd));
  f.emplace_back("custom_payload", dg(d, "custom_payload"));
  f.emplace_back("forward_payload", dg(d, "forward_payload"));
  Value resp = dg(d, "response_destination");
  f.emplace_back("response_destination", resp.is_null() ? Value::null() : av_addr(resp));
  f.emplace_back("marketplace", (has(d, "marketplace") && !dg(d, "marketplace").is_null()) ? dg(d, "marketplace") : Value::null());
  f.emplace_back("marketplace_address", (has(d, "marketplace_address") && !dg(d, "marketplace_address").is_null()) ? av_addr(dg(d, "marketplace_address")) : Value::null());
  f.emplace_back("real_prev_owner", (has(d, "real_prev_owner") && !dg(d, "real_prev_owner").is_null()) ? av_addr(dg(d, "real_prev_owner")) : Value::null());
  f.emplace_back("payout_amount", has(d, "payout_amount") ? av_amount(dg(d, "payout_amount")) : Value::null());
  f.emplace_back("payout_comment_encrypted", dg(d, "payout_comment_encrypted"));
  f.emplace_back("payout_comment_encoded", dg(d, "payout_comment_encoded"));
  f.emplace_back("payout_comment", dg(d, "payout_comment"));
  f.emplace_back("royalty_amount", has(d, "royalty_amount") ? av_amount(dg(d, "royalty_amount")) : Value::null());
  a.nft_transfer_data = mkdict(std::move(f));
  Value::Fields l;
  l.emplace_back("marketplace_fee_address", has(d, "payout_address") ? av_addr(dg(d, "payout_address")) : Value::null());
  l.emplace_back("royalty_address", has(d, "royalty_address") ? av_addr(dg(d, "royalty_address")) : Value::null());
  a.nft_listing_data = mkdict(std::move(l));
}

void fill_dedust_deposit(const Value &d, Action &a) {
  a.type = "dex_deposit_liquidity";
  a.source = av_addr(dg(d, "sender"));
  a.destination = av_addr(dg(d, "pool_address"));
  a.destination_secondary = av_addr(dg(d, "deposit_contract"));
  std::vector<Value> excesses;
  Value ve = dg(d, "vault_excesses");
  if (ve.items) {
    for (const Value &e : *ve.items) {
      Value::Fields ef;
      Value a0 = Value::null(), a1 = Value::null();
      if (e.items && e.items->size() >= 2) {
        a0 = (*e.items)[0];
        a1 = (*e.items)[1];
      }
      ef.emplace_back("asset", av_addr(a0));
      ef.emplace_back("amount", av_amount(a1));
      excesses.push_back(mkdict(std::move(ef)));
    }
  }
  Value::Fields f;
  f.emplace_back("dex", dg(d, "dex"));
  f.emplace_back("asset1", av_addr(dg(d, "asset_1")));
  f.emplace_back("amount1", av_amount(dg(d, "amount_1")));
  f.emplace_back("asset2", av_addr(dg(d, "asset_2")));
  f.emplace_back("amount2", av_amount(dg(d, "amount_2")));
  f.emplace_back("user_jetton_wallet_1", av_addr(dg(d, "user_jetton_wallet_1")));
  f.emplace_back("user_jetton_wallet_2", av_addr(dg(d, "user_jetton_wallet_2")));
  f.emplace_back("lp_tokens_minted", av_amount(dg(d, "lp_tokens_minted")));
  f.emplace_back("target_asset_1", av_addr(dg(d, "target_asset_1")));
  f.emplace_back("target_amount_1", av_amount(dg(d, "target_amount_1")));
  f.emplace_back("target_asset_2", av_addr(dg(d, "target_asset_2")));
  f.emplace_back("target_amount_2", av_amount(dg(d, "target_amount_2")));
  f.emplace_back("vault_excesses", Value::make_list(std::move(excesses)));
  a.dex_deposit_liquidity_data = mkdict(std::move(f));
}

void fill_dedust_deposit_partial(const Value &d, Action &a) {
  a.type = "dex_deposit_liquidity";
  a.source = av_addr(dg(d, "sender"));
  a.destination_secondary = av_addr(dg(d, "deposit_contract"));
  Value::Fields f;
  f.emplace_back("dex", dg(d, "dex"));
  f.emplace_back("asset1", av_addr(dg(d, "asset_1")));
  f.emplace_back("amount1", av_amount(dg(d, "amount_1")));
  f.emplace_back("asset2", av_addr(dg(d, "asset_2")));
  f.emplace_back("amount2", av_amount(dg(d, "amount_2")));
  f.emplace_back("user_jetton_wallet_1", av_addr(dg(d, "user_jetton_wallet_1")));
  f.emplace_back("user_jetton_wallet_2", av_addr(dg(d, "user_jetton_wallet_2")));
  f.emplace_back("lp_tokens_minted", Value::null());
  f.emplace_back("target_asset_1", av_addr(dg(d, "target_asset_1")));
  f.emplace_back("target_amount_1", av_amount(dg(d, "target_amount_1")));
  f.emplace_back("target_asset_2", av_addr(dg(d, "target_asset_2")));
  f.emplace_back("target_amount_2", av_amount(dg(d, "target_amount_2")));
  a.dex_deposit_liquidity_data = mkdict(std::move(f));
}

// T12.2: DeDust CPMM V2 reward claim (block_tree_serializer _fill_dedust_v2_claim_reward).
// Reuses the dex_withdraw_liquidity_data composite; reward_index is carried in
// the block data but is NOT serialized (upstream v1.3 contract).
void fill_dedust_v2_claim_reward(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.destination = av_addr(dg(d, "pool"));
  a.destination_secondary = av_addr(dg(d, "position"));
  Value::Fields f;
  f.emplace_back("dex", dg(d, "dex"));
  f.emplace_back("amount1", av_amount(dg(d, "amount")));
  f.emplace_back("amount2", Value::null());
  f.emplace_back("asset1_out", av_addr(dg(d, "asset")));
  f.emplace_back("asset2_out", Value::null());
  f.emplace_back("user_jetton_wallet_1", av_addr(dg(d, "user_wallet")));
  f.emplace_back("user_jetton_wallet_2", Value::null());
  f.emplace_back("dex_wallet_1", av_addr(dg(d, "dex_wallet")));
  f.emplace_back("dex_wallet_2", Value::null());
  f.emplace_back("dex_jetton_wallet_1", av_addr(dg(d, "dex_jetton_wallet")));
  f.emplace_back("dex_jetton_wallet_2", Value::null());
  f.emplace_back("is_refund", Value::make_bool(false));
  f.emplace_back("lp_tokens_burnt", Value::null());
  a.dex_withdraw_liquidity_data = mkdict(std::move(f));
}

void fill_tonco_deposit(const Value &d, Action &a) {
  a.type = "dex_deposit_liquidity";
  a.source = av_addr(dg(d, "sender"));
  Value sw1 = dg(d, "sender_wallet_1"), sw2 = dg(d, "sender_wallet_2");
  a.source_secondary = av_addr(!sw1.is_null() ? sw1 : sw2);
  a.destination = av_addr(dg(d, "pool"));
  a.destination_secondary = av_addr(dg(d, "account_contract"));
  std::vector<Value> excesses;
  Value ve = dg(d, "excesses");
  if (ve.items) {
    for (const Value &e : *ve.items) {
      Value::Fields ef;
      Value a0 = Value::null(), a1 = Value::null();
      if (e.items && e.items->size() >= 2) {
        a0 = (*e.items)[0];
        a1 = (*e.items)[1];
      }
      ef.emplace_back("asset", av_addr(a0));
      ef.emplace_back("amount", av_amount(a1));
      excesses.push_back(mkdict(std::move(ef)));
    }
  }
  // pick the first/second present (non-null amount) leg
  Value amt1 = dg(d, "amount_1"), as1 = dg(d, "asset_1");
  Value amt2 = dg(d, "amount_2"), as2 = dg(d, "asset_2");
  Value actual_amount_1 = Value::null(), actual_asset_1 = Value::null();
  Value actual_amount_2 = Value::null(), actual_asset_2 = Value::null();
  Value pairs_amt[2] = {amt1, amt2};
  Value pairs_asset[2] = {as1, as2};
  for (int i = 0; i < 2; i++) {
    if (pairs_amt[i].is_null()) continue;
    if (actual_amount_1.is_null()) {
      actual_amount_1 = pairs_amt[i];
      actual_asset_1 = pairs_asset[i];
    } else {
      actual_amount_2 = pairs_amt[i];
      actual_asset_2 = pairs_asset[i];
    }
  }
  Value::Fields f;
  f.emplace_back("dex", Value::make_str("tonco"));
  f.emplace_back("amount1", av_amount(actual_amount_1));
  f.emplace_back("amount2", av_amount(actual_amount_2));
  f.emplace_back("asset1", av_addr(actual_asset_1));
  f.emplace_back("asset2", av_addr(actual_asset_2));
  f.emplace_back("user_jetton_wallet_1", av_addr(sw1));
  f.emplace_back("user_jetton_wallet_2", av_addr(sw2));
  f.emplace_back("lp_tokens_minted", av_amount(dg(d, "lp_tokens_minted")));
  f.emplace_back("tick_lower", dg(d, "tick_lower"));
  f.emplace_back("tick_upper", dg(d, "tick_upper"));
  f.emplace_back("nft_index", dg(d, "nft_index"));
  f.emplace_back("nft_address", av_addr(dg(d, "nft_address")));
  f.emplace_back("target_amount_1", av_amount(dg(d, "position_amount_1")));
  f.emplace_back("target_amount_2", av_amount(dg(d, "position_amount_2")));
  f.emplace_back("target_asset_1", av_addr(dg(d, "asset_1")));
  f.emplace_back("target_asset_2", av_addr(dg(d, "asset_2")));
  f.emplace_back("vault_excesses", Value::make_list(std::move(excesses)));
  a.dex_deposit_liquidity_data = mkdict(std::move(f));
}

void fill_evaa_withdraw(const Value &d, Action &a, bool block_failed) {
  a.source = av_addr(dg(d, "owner"));
  a.destination = av_addr(dg(d, "recipient"));
  a.destination_secondary = av_addr(dg(d, "owner_contract"));
  a.amount = av_amount(dg(d, "amount"));
  a.asset = av_addr(dg(d, "asset"));
  Value succ = dg(d, "is_success");
  a.success = succ.t == VType::Bool && succ.boolean;
  if (block_failed) a.success = false;
  Value::Fields f;
  f.emplace_back("is_ton", dg(d, "is_ton"));
  Value rjw = dg(d, "recipient_jetton_wallet");
  f.emplace_back("recipient_jetton_wallet", rjw.is_null() ? Value::null() : av_addr(rjw));
  Value mjw = dg(d, "master_jetton_wallet");
  f.emplace_back("master_jetton_wallet", mjw.is_null() ? Value::null() : av_addr(mjw));
  f.emplace_back("fail_reason", dg(d, "fail_reason"));
  f.emplace_back("master", av_addr(dg(d, "master")));
  f.emplace_back("asset_id", av_hex(dg(d, "asset_id")));
  a.evaa_withdraw_data = mkdict(std::move(f));
}

void fill_jvault_stake(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.source_secondary = av_addr(dg(d, "sender_wallet"));
  a.asset = av_addr(dg(d, "asset"));
  a.destination = av_addr(dg(d, "staking_pool"));
  a.amount = av_amount(dg(d, "staked_amount"));
  Value::Fields f;
  f.emplace_back("period", dg(d, "period"));
  f.emplace_back("stake_wallet", av_addr(dg(d, "stake_wallet")));
  a.jvault_stake_data = mkdict(std::move(f));
}

void fill_jvault_unstake(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.source_secondary = av_addr(dg(d, "stake_wallet"));
  a.destination = av_addr(dg(d, "staking_pool"));
  a.amount = av_amount(dg(d, "unstaked_amount"));
  a.opcode = dg(d, "exit_code");
  a.asset = av_addr(dg(d, "asset"));
  a.asset2 = av_addr(dg(d, "jvault_asset"));
}

void fill_jvault_unstake_request(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.source_secondary = av_addr(dg(d, "stake_wallet"));
  a.destination = av_addr(dg(d, "staking_pool"));
  a.amount = av_amount(dg(d, "requested_amount"));
  a.asset = av_addr(dg(d, "asset"));
  a.asset2 = av_addr(dg(d, "jvault_asset"));
  a.opcode = dg(d, "exit_code");
}

void fill_jvault_claim(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.source_secondary = av_addr(dg(d, "stake_wallet"));
  a.destination = av_addr(dg(d, "staking_pool"));
  std::vector<Value> jettons;
  Value cj = dg(d, "claimed_jettons");
  if (cj.items) {
    for (const Value &j : *cj.items) jettons.push_back(av_addr(j));
  }
  Value::Fields f;
  f.emplace_back("claimed_jettons", Value::make_list(std::move(jettons)));
  f.emplace_back("claimed_amounts", dg(d, "claimed_amounts"));
  a.jvault_claim_data = mkdict(std::move(f));
}

void fill_ethena_deposit(const Value &d, Action &a) {
  a.type = "stake_deposit";
  a.source = av_addr(dg(d, "source"));
  a.source_secondary = av_addr(dg(d, "user_jetton_wallet"));
  a.destination = av_addr(dg(d, "pool"));
  a.amount = av_amount(dg(d, "value"));
  a.asset = av_addr(dg(d, "asset"));
  a.asset2 = av_addr(dg(d, "source_asset"));
  Value::Fields f;
  f.emplace_back("provider", Value::make_str("ethena"));
  f.emplace_back("tokens_minted", av_amount(dg(d, "tokens_minted")));
  a.staking_data = mkdict(std::move(f));
}

void fill_layerzero_receive(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.destination = av_addr(dg(d, "oapp"));
  a.destination_secondary = av_addr(dg(d, "channel"));
  a.layerzero_packet_data = dg(d, "packet_data");  // record dict passthrough
}

void fill_coffee_create_vault(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.destination = av_addr(dg(d, "vault"));
  a.asset = av_addr(dg(d, "asset"));
  a.value = av_amount(dg(d, "amount"));
}

void fill_vesting_send_message(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.destination = av_addr(dg(d, "vesting"));
  a.destination_secondary = av_addr(dg(d, "message_destination"));
  a.amount = av_amount(dg(d, "message_value"));
  Value succ = dg(d, "success");  // the message was actually sent (upstream #430)
  a.success = succ.t == VType::Bool && succ.boolean;
  Value::Fields f;
  f.emplace_back("query_id", dg(d, "query_id"));
  f.emplace_back("message_boc", dg(d, "message_boc_str"));
  a.vesting_send_message_data = mkdict(std::move(f));
}

void fill_tgbtc_mint(const Value &d, Action &a) {
  a.type = "tgbtc_mint";
  Value cr = dg(d, "crippled");
  if (cr.t == VType::Bool && cr.boolean) a.type += "_fallback";
  a.source = av_addr(dg(d, "sender"));
  a.destination = av_addr(dg(d, "recipient"));
  a.amount = av_amount(dg(d, "amount"));
  a.asset = av_addr(dg(d, "asset"));
  Value succ = dg(d, "success");
  a.success = succ.t == VType::Bool && succ.boolean;
  Value::Fields ex;
  ex.emplace_back("btc_txid", dg(d, "bitcoin_txid"));
  a.extra = mkdict(std::move(ex));
  a.source_secondary = av_addr(dg(d, "teleport_contract"));
  a.destination_secondary = av_addr(dg(d, "recipient_wallet"));
}

// --- W6 bucket-B fills (em2-writeback-design.md §2.4) ------------------------
// Btypes whose MATCHER runs and produces a block, but which had no fill_action
// branch — so build_action returned false and the whole row vanished from the
// payload even though the protocol HAD been recognised. Zero of these fire on
// the 165-fixture corpus, so the A/B twin cannot cover them; each is gated
// hermetically instead (--actions-msgpack-test), with the data field names
// taken from the IR's own `out`/`switch` field lists and the column set from
// the matching Python _fill_*.

void fill_dns_renew(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "source"));
  a.destination = av_addr(dg(d, "destination"));
  a.asset = av_addr(dg(d, "collection_address"));
}

// Python bytes.hex(): lowercase, unseparated, no 0x prefix. td::buffer_to_hex
// is UPPERCASE and must NOT be substituted — Go takes both these fields as
// *string, so the case difference would sail past the decoder and land in front
// of API consumers as a silently different key.
Value bytes_hex(const Value &v) {
  if (v.t != VType::Bytes) {
    return Value::null();
  }
  static const char kHex[] = "0123456789abcdef";
  std::string out(v.str.size() * 2, '\0');
  for (std::size_t i = 0; i < v.str.size(); i++) {
    const auto c = static_cast<unsigned char>(v.str[i]);
    out[2 * i] = kHex[c >> 4];
    out[2 * i + 1] = kHex[c & 15];
  }
  return Value::make_str(std::move(out));
}

// _fill_change_dns_record_action. The record dict the parser produces carries
// {schema, address, flags, dns_text} exactly as Python's does, so the branching
// is a transcription — but the KEY SET is Python's, not the branch's: it always
// emits value_schema/flags/address/key and appends `value` only for a schema it
// recognises. `address` is dead on both sides (Python initialises it to None and
// never assigns it, and Go has no such field); it is kept so the decoded map
// matches Python's key for key.
void fill_change_dns(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "source"));
  a.destination = av_addr(dg(d, "destination"));
  a.asset = av_addr(dg(d, "collection_address"));

  const Value rec = dg(d, "value");  // the parsed dns_record_data
  const Value schema = dg(rec, "schema");
  const std::string s = schema.t == VType::Str ? schema.str : std::string();

  Value value_field = Value::null();
  bool has_value_field = true;
  Value flags = Value::null();
  if (s == "DNSNextResolver" || s == "DNSSmcAddress") {
    value_field = av_addr(dg(rec, "address"));
  } else if (s == "DNSAdnlAddress") {
    value_field = bytes_hex(dg(rec, "address"));
    flags = dg(rec, "flags");
  } else if (s == "DNSStorageAddress") {
    value_field = bytes_hex(dg(rec, "address"));
  } else if (s == "DNSText") {
    value_field = dg(rec, "dns_text");
  } else {
    has_value_field = false;  // "Unknown": Python never creates the key
  }
  if (s == "DNSSmcAddress") {
    flags = dg(rec, "flags");  // a SECOND standalone `if` in Python, not an elif
  }

  Value::Fields f;
  f.emplace_back("value_schema", schema);
  // Int, not Str: the serializer stringifies every non-excluded int inside a
  // composite, which is what Python does and what Go's *string + ParseInt
  // expects — this one agrees on both sides, unlike the §2.6 fields.
  f.emplace_back("flags", flags);
  f.emplace_back("address", Value::null());
  f.emplace_back("key", bytes_hex(dg(d, "key")));
  if (has_value_field) {
    f.emplace_back("value", value_field);
  }
  a.change_dns_record_data = mkdict(std::move(f));
}

// _fill_delete_dns_record_action: the same column with everything but the key
// nulled — a deletion names the record it removes and nothing else.
void fill_delete_dns(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "source"));
  a.destination = av_addr(dg(d, "destination"));
  a.asset = av_addr(dg(d, "collection_address"));
  Value::Fields f;
  f.emplace_back("value_schema", Value::null());
  f.emplace_back("flags", Value::null());
  f.emplace_back("address", Value::null());
  f.emplace_back("key", bytes_hex(dg(d, "key")));
  a.change_dns_record_data = mkdict(std::move(f));
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
  Value::Fields f;
  f.emplace_back("provider", Value::make_str("nominator"));
  a.staking_data = mkdict(std::move(f));
  a.source = av_addr(dg(d, "source"));
  a.destination = av_addr(dg(d, "pool"));
}

void fill_tonstakers_withdraw(const Value &d, Action &a) {
  a.type = "stake_withdrawal";
  a.source = av_addr(dg(d, "stake_holder"));
  a.destination = av_addr(dg(d, "pool"));
  a.amount = av_amount(dg(d, "amount"));
  a.asset = av_addr(dg(d, "asset"));
  Value::Fields f;
  f.emplace_back("provider", Value::make_str("liquid_staking"));
  f.emplace_back("ts_nft", av_addr(dg(d, "burnt_nft")));
  f.emplace_back("tokens_burnt", av_amount(dg(d, "tokens_burnt")));
  a.staking_data = mkdict(std::move(f));
}

void fill_tonstakers_withdraw_request(const Value &d, Action &a) {
  a.type = "stake_withdrawal_request";
  a.source = av_addr(dg(d, "source"));
  a.source_secondary = av_addr(dg(d, "tsTON_wallet"));
  a.destination = av_addr(dg(d, "pool"));
  a.amount = av_amount(dg(d, "tokens_burnt"));
  a.asset = av_addr(dg(d, "asset"));
  Value::Fields f;
  f.emplace_back("provider", Value::make_str("liquid_staking"));
  f.emplace_back("ts_nft", av_addr(dg(d, "minted_nft")));
  a.staking_data = mkdict(std::move(f));
}

// The BARE dex_deposit_liquidity (stonfi v2 provide), distinct from the dedust
// deposit fills above: same composite COLUMN, different producing matcher and a
// different data field spelling (amount_1 here, amount1 there).
void fill_dex_deposit_liquidity(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.destination = av_addr(dg(d, "pool"));
  Value::Fields f;
  f.emplace_back("dex", dg(d, "dex"));
  f.emplace_back("amount1", av_amount(dg(d, "amount_1")));
  f.emplace_back("amount2", av_amount(dg(d, "amount_2")));
  f.emplace_back("asset1", av_addr(dg(d, "asset_1")));
  f.emplace_back("asset2", av_addr(dg(d, "asset_2")));
  f.emplace_back("user_jetton_wallet_1", av_addr(dg(d, "sender_wallet_1")));
  f.emplace_back("user_jetton_wallet_2", av_addr(dg(d, "sender_wallet_2")));
  f.emplace_back("lp_tokens_minted", av_amount(dg(d, "lp_tokens_minted")));
  a.dex_deposit_liquidity_data = mkdict(std::move(f));
}

// The BARE dex_withdraw_liquidity (dedust withdraw / stonfi v2 withdraw).
void fill_dex_withdraw_liquidity(const Value &d, Action &a) {
  a.source = av_addr(dg(d, "sender"));
  a.source_secondary = av_addr(dg(d, "sender_wallet"));
  a.destination = av_addr(dg(d, "pool"));
  a.asset = av_addr(dg(d, "asset"));
  Value::Fields f;
  f.emplace_back("dex", dg(d, "dex"));
  f.emplace_back("amount1", av_amount(dg(d, "amount1_out")));
  f.emplace_back("amount2", av_amount(dg(d, "amount2_out")));
  f.emplace_back("asset1_out", av_addr(dg(d, "asset1_out")));
  f.emplace_back("asset2_out", av_addr(dg(d, "asset2_out")));
  f.emplace_back("user_jetton_wallet_1", av_addr(dg(d, "wallet1")));
  f.emplace_back("user_jetton_wallet_2", av_addr(dg(d, "wallet2")));
  f.emplace_back("dex_jetton_wallet_1", av_addr(dg(d, "dex_jetton_wallet_1")));
  f.emplace_back("dex_wallet_1", av_addr(dg(d, "dex_wallet_1")));
  f.emplace_back("dex_wallet_2", av_addr(dg(d, "dex_wallet_2")));
  f.emplace_back("dex_jetton_wallet_2", av_addr(dg(d, "dex_jetton_wallet_2")));
  f.emplace_back("is_refund", dg(d, "is_refund"));
  f.emplace_back("lp_tokens_burnt", av_amount(dg(d, "lp_tokens_burnt")));
  a.dex_withdraw_liquidity_data = mkdict(std::move(f));
}

// getgems / telegram NFT purchase. Sets nft_transfer_data ONLY — unlike
// fill_nft_transfer, which also writes nft_listing_data.
void fill_nft_purchase(const Value &d, Action &a) {
  // Python guards the source assignment on prev_owner being present; av_addr
  // already answers Null for a missing or addr_none value, so the guard is a
  // no-op and the unconditional form is equivalent.
  a.source = av_addr(dg(d, "prev_owner"));
  a.destination = av_addr(dg(d, "new_owner"));
  a.asset_secondary = av_addr(dg(d, "nft_address"));
  a.asset = av_addr(dg(d, "collection_address"));
  Value::Fields f;
  f.emplace_back("query_id", dg(d, "query_id"));
  f.emplace_back("is_purchase", Value::make_bool(true));
  f.emplace_back("price", av_amount(dg(d, "price")));
  f.emplace_back("nft_item_index", dg(d, "nft_index"));
  f.emplace_back("forward_amount", av_amount(dg(d, "forward_amount")));
  f.emplace_back("custom_payload", dg(d, "custom_payload"));
  f.emplace_back("forward_payload", dg(d, "forward_payload"));
  f.emplace_back("response_destination", av_addr(dg(d, "response_destination")));
  f.emplace_back("marketplace", dg(d, "marketplace"));
  f.emplace_back("marketplace_address", av_addr(dg(d, "marketplace_address")));
  f.emplace_back("real_prev_owner", av_addr(dg(d, "real_prev_owner")));
  f.emplace_back("payout_amount", av_amount(dg(d, "payout_amount")));
  f.emplace_back("payout_comment_encrypted", dg(d, "payout_comment_encrypted"));
  f.emplace_back("payout_comment_encoded", dg(d, "payout_comment_encoded"));
  f.emplace_back("payout_comment", dg(d, "payout_comment"));
  a.nft_transfer_data = mkdict(std::move(f));
}

// --- basic-actions fallback fills (Step 2c; block_tree_serializer.py) --------
// Reached ONLY on the FALLBACK path (ClassifyResult.failure) — the main dump's
// `pending` is matcher-produced composites, never these leaf btypes. Corpus has
// no failing traces, so this path is ungated (regression-gated only).
void fill_call_contract(const Value &d, Action &a) {
  a.opcode = dg(d, "opcode");
  a.value = av_amount(dg(d, "value"));
  a.source = av_addr(dg(d, "source"));
  a.destination = av_addr(dg(d, "destination"));
  // value_extra_currencies is not a rendered column; `extra_currencies` is
  // absent from the C++ leaf data (§10 residual) — Python defaults it to {}.
}

void fill_ton_transfer(const Value &d, Action &a) {
  a.value = av_amount(dg(d, "value"));
  a.source = av_addr(dg(d, "source"));
  a.destination = av_addr(dg(d, "destination"));
  Value::Fields f;
  f.emplace_back("content", dg(d, "comment"));  // leaf_comment_info already strips U+0000
  f.emplace_back("encrypted", dg(d, "encrypted"));
  a.ton_transfer_data = mkdict(std::move(f));
}

// Dispatch a produced block to its fill (block_to_action match). Returns false
// for a btype outside the ported set (skip-table entry).
bool fill_action(Block *b, Action &a) {
  const Value &d = b->data;
  const std::string &t = b->btype;
  if (t == "call_contract" || t == "contract_deploy") fill_call_contract(d, a);
  else if (t == "ton_transfer") fill_ton_transfer(d, a);
  else if (t == "jetton_transfer") fill_jetton_transfer(d, a);
  else if (t == "jetton_burn") fill_jetton_burn(d, a);
  else if (t == "jetton_mint") fill_jetton_mint(d, a);
  else if (t == "jetton_swap") fill_jetton_swap(d, a);
  else if (t == "nft_transfer") fill_nft_transfer(d, a);
  else if (t == "dedust_deposit_liquidity") fill_dedust_deposit(d, a);
  else if (t == "dedust_deposit_liquidity_partial") fill_dedust_deposit_partial(d, a);
  else if (t == "tonco_deposit_liquidity") fill_tonco_deposit(d, a);
  else if (t == "dedust_v2_claim_reward") fill_dedust_v2_claim_reward(d, a);
  else if (t == "evaa_withdraw") fill_evaa_withdraw(d, a, b->failed);
  else if (t == "jvault_stake") fill_jvault_stake(d, a);
  else if (t == "jvault_unstake") fill_jvault_unstake(d, a);
  else if (t == "jvault_unstake_request") fill_jvault_unstake_request(d, a);
  else if (t == "jvault_claim") fill_jvault_claim(d, a);
  else if (t == "ethena_deposit") fill_ethena_deposit(d, a);
  else if (t == "layerzero_receive") fill_layerzero_receive(d, a);
  else if (t == "coffee_create_vault") fill_coffee_create_vault(d, a);
  else if (t == "vesting_send_message") fill_vesting_send_message(d, a);
  else if (t == "tgbtc_mint") fill_tgbtc_mint(d, a);
  // W6 bucket B: matcher-produced btypes that used to fall through to `return
  // false` and lose the whole row.
  else if (t == "renew_dns") fill_dns_renew(d, a);
  else if (t == "nominator_pool_withdraw_request") fill_nominator_pool_withdraw_request(d, a);
  else if (t == "tonstakers_withdraw") fill_tonstakers_withdraw(d, a);
  else if (t == "tonstakers_withdraw_request") fill_tonstakers_withdraw_request(d, a);
  else if (t == "dex_deposit_liquidity") fill_dex_deposit_liquidity(d, a);
  else if (t == "dex_withdraw_liquidity") fill_dex_withdraw_liquidity(d, a);
  // nft_purchase: the former NAMED SKIP, REVERSED in W6 by the lead.
  // The old note said nft_purchase had zero corpus coverage and so nothing to
  // A/B-gate, and left it unported. That reasoning does not survive EM2: the
  // matcher runs and Python emits the row, so leaving it out does not avoid a
  // diff — it MANUFACTURES one in `real-mismatch`, the one phase-1 bucket that
  // must read zero, on traffic (GetGems) that is plentiful live even though the
  // fixture corpus has none of it. Gated hermetically instead
  // (--actions-msgpack-test), like the other W6 fills.
  else if (t == "nft_purchase") fill_nft_purchase(d, a);
  else if (t == "change_dns") fill_change_dns(d, a);
  else if (t == "delete_dns") fill_delete_dns(d, a);
  // STILL UNPORTED, deliberately: dns_purchase is bucket C, not B — NO matcher
  // in ir/mch_ir.json produces it, so there is nothing to fill.
  else return false;
  return true;
}
}  // namespace

bool build_action(Block *b, Action &a) {
  a.type = b->btype;
  a.success = !b->failed;
  // action_id: sha256(root_event_node msg_hash|tx_hash + btype), base64.
  const EventNode *root = nullptr;
  for (const EventNode *n : b->event_nodes) {
    if (root == nullptr || n->lt() < root->lt()) root = n;
  }
  std::string key;
  if (root != nullptr) {
    key = (root->msg != nullptr) ? root->msg->msg_hash : root->tx_hash();
  }
  key += b->btype;
  std::string digest = td::sha256(td::Slice(key));
  a.action_id = td::base64_encode(td::Slice(digest));
  // tx_hashes (base set, event_nodes only) + lt/utime ranges + mc_seqno_end.
  std::set<std::string> ths;
  std::int64_t maxlt = std::numeric_limits<std::int64_t>::min();
  std::int64_t minu = std::numeric_limits<std::int64_t>::max();
  std::int64_t maxu = std::numeric_limits<std::int64_t>::min();
  std::int64_t seqmax = std::numeric_limits<std::int64_t>::min();
  for (const EventNode *n : b->event_nodes) {
    ths.insert(n->tx_hash());
    std::int64_t tlt = n->tx != nullptr ? n->tx->lt : 0;
    maxlt = std::max(maxlt, tlt);
    minu = std::min(minu, node_utime(n));
    maxu = std::max(maxu, n->tx != nullptr ? n->tx->now : 0);
    if (n->tx != nullptr) seqmax = std::max(seqmax, n->tx->mc_block_seqno);
  }
  // tx_hashes extension by initiating_event_node (block_to_action, Step 2a).
  if (b->initiating_event_node != nullptr) {
    ths.insert(b->initiating_event_node->tx_hash());
  }
  a.tx_hashes.assign(ths.begin(), ths.end());
  a.start_lt = b->min_lt;
  a.end_lt = b->event_nodes.empty() ? 0 : maxlt;
  a.start_utime = b->event_nodes.empty() ? 0 : minu;
  a.end_utime = b->event_nodes.empty() ? 0 : maxu;
  a.mc_seqno_end = b->event_nodes.empty() ? 0 : seqmax;
  if (!fill_action(b, a)) return false;

  // accounts assembly (block_to_action tail, Step 2a; role-less — no role
  // system on this branch). base = each event_node's tx account; + source/
  // source_secondary; + destination/destination_secondary UNLESS ghost; +
  // the initiating tx account (non-tick_tock). Dedup, drop nulls. Directional
  // fields are av_addr Str values (or Null → skipped).
  std::vector<std::string> accts;
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

Action create_unknown_action(const Trace &trace) {
  Action a;
  a.type = "unknown";
  a.action_id = trace.trace_id;
  // Straight off the Trace, exactly as Python takes them (:1692-1703). Every
  // loader fills these: the fixture path from the trace header, the PRODUCTION
  // SchemaTraceLoader by deriving them (fill_trace_aggregates, TraceLoader.h).
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
  // Python builds both from an unordered list/set comprehension; every consumer
  // and both A/B comparators sort, so the sorted form is the deterministic one.
  std::sort(a.tx_hashes.begin(), a.tx_hashes.end());
  a.accounts.assign(accts.begin(), accts.end());
  return a;
}

}  // namespace mch
