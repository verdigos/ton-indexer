#include "Classify.h"

#include "BlockTree.h"
#include "BuildDriver.h"
#include "ClassifyCore.h"
#include "BuildRuntime.h"
#include "ExprRuntime.h"
#include "IrLoader.h"
#include "MsgParse.h"

#include "td/utils/base64.h"
#include "td/utils/crypto.h"

#include "common/refint.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace mch {

namespace {

// Render + print a collected CoreAction from its FINAL (post-pass) state. The
// aux/flows strings (fire-time) are rendered HERE from the core's raw block/
// flow lists — the core stays rendering-agnostic (§11 item 2 isolation).
void emit_action(const CoreAction &a) {
  std::vector<std::string> ck;
  for (Block *cb : a.produced->children_blocks) ck.push_back(block_key(cb));
  std::sort(ck.begin(), ck.end());
  std::string consumed_s = "[";
  for (std::size_t i = 0; i < ck.size(); i++) {
    if (i) consumed_s += ",";
    consumed_s += ck[i];
  }
  consumed_s += "]";

  std::string aux_s = "[";
  for (std::size_t i = 0; i < a.aux.size(); i++) {
    if (i) aux_s += ",";
    aux_s += block_key(a.aux[i]);
  }
  aux_s += "]";

  std::string flows_s = "[";
  for (std::size_t i = 0; i < a.flows.size(); i++) {
    if (i) flows_s += ",";
    flows_s += a.flows[i].kind + "(";
    for (std::size_t j = 0; j < a.flows[i].args.size(); j++) {
      if (j) flows_s += ",";
      flows_s += render_value(a.flows[i].args[j]);
    }
    flows_s += ")";
  }
  flows_s += "]";

  std::printf("ACTION %s anchor=%s btype=%s failed=%d broken=%d data=%s flows=%s "
              "consumed=%s auto=%s\n",
              a.matcher_name.c_str(), block_key(a.anchor).c_str(), a.produced->btype.c_str(),
              a.produced->failed ? 1 : 0, a.produced->broken ? 1 : 0,
              render_value(a.produced->data).c_str(), flows_s.c_str(), consumed_s.c_str(),
              aux_s.c_str());
}

// ===========================================================================
// Phase E part 2: C++ Action production (block_tree_serializer.py port).
//
// Scope: the composite btypes that FIRE in the corpus classify dump (18 of
// them), rendered per surviving post-pass PendingAction — the same block set
// the classify dump emits. Basic leaf actions (call_contract/ton_transfer/
// contract_deploy) and the serializer's spine-walk / child-recursion wrapper
// (parent_action_id, ancestor_type, v1_ops) are OUT of scope: the C++ leaf
// .data is a subset (§10 hazard) and those wrappers add no fill coverage.
//
// STAGED OUT (reported): accounts + role_assignment + the block_to_action
// tx_hashes extension by initiating_event_node — the C++ Block has no
// initiating_event_node member, so accounts/roles cannot be reproduced
// deterministically (Phase G production wiring). tx_hashes below is the BASE
// set (event_nodes only), computed identically on both A/B sides. trace_id /
// trace_* / finality are constant-per-fixture and not rendered.
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

// The rendered Action (fixed schema; unset fields are Null, rendered "null").
struct Action {
  std::string action_id;
  std::string type;
  bool success{true};
  std::int64_t start_lt{0}, end_lt{0}, start_utime{0}, end_utime{0}, mc_seqno_end{0};
  std::vector<std::string> tx_hashes;
  std::vector<std::string> accounts;  // Step 2a: role-less action_accounts set
  Value source, source_secondary, destination, destination_secondary;
  Value asset, asset_secondary, asset2, asset2_secondary;
  Value amount, value, opcode;
  Value ton_transfer_data, jetton_transfer_data, jetton_swap_data, nft_transfer_data,
      nft_listing_data, dex_deposit_liquidity_data, staking_data, evaa_withdraw_data,
      vesting_send_message_data, layerzero_packet_data, jvault_stake_data, jvault_claim_data,
      extra;
};

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
  // NAMED SKIP — nft_purchase / dns_purchase (getgems_nft_purchase +
  // telegram_nft_purchase): the Python fill _fill_nft_purchase_action is now
  // _dget-safe (dual-dispatch wave), but nft_purchase has ZERO corpus coverage
  // (no getgems/telegram purchase traces fire), so there is nothing to A/B-gate
  // — deliberately NOT ported. Add a fill here (Python fill already handles the
  // dict) if a purchase trace ever enters the corpus.
  else return false;
  return true;
}

// _base_block_to_action + block_to_action dispatch, over a produced block.
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

std::string render_action(const Action &a) {
  Value::Fields f;
  f.emplace_back("id", Value::make_str(a.action_id));
  f.emplace_back("type", Value::make_str(a.type));
  f.emplace_back("success", Value::make_bool(a.success));
  f.emplace_back("start_lt", Value::make_int64(a.start_lt));
  f.emplace_back("end_lt", Value::make_int64(a.end_lt));
  f.emplace_back("start_utime", Value::make_int64(a.start_utime));
  f.emplace_back("end_utime", Value::make_int64(a.end_utime));
  f.emplace_back("mc_seqno_end", Value::make_int64(a.mc_seqno_end));
  std::vector<std::string> th = a.tx_hashes;
  std::sort(th.begin(), th.end());
  std::vector<Value> thv;
  for (const std::string &s : th) thv.push_back(Value::make_str(s));
  f.emplace_back("tx_hashes", Value::make_list(std::move(thv)));
  std::vector<std::string> ac = a.accounts;
  std::sort(ac.begin(), ac.end());
  std::vector<Value> acv;
  for (const std::string &s : ac) acv.push_back(Value::make_str(s));
  f.emplace_back("accounts", Value::make_list(std::move(acv)));
  f.emplace_back("source", a.source);
  f.emplace_back("source_secondary", a.source_secondary);
  f.emplace_back("destination", a.destination);
  f.emplace_back("destination_secondary", a.destination_secondary);
  f.emplace_back("asset", a.asset);
  f.emplace_back("asset_secondary", a.asset_secondary);
  f.emplace_back("asset2", a.asset2);
  f.emplace_back("asset2_secondary", a.asset2_secondary);
  f.emplace_back("amount", a.amount);
  f.emplace_back("value", a.value);
  f.emplace_back("opcode", a.opcode);
  f.emplace_back("ton_transfer_data", a.ton_transfer_data);
  f.emplace_back("jetton_transfer_data", a.jetton_transfer_data);
  f.emplace_back("jetton_swap_data", a.jetton_swap_data);
  f.emplace_back("nft_transfer_data", a.nft_transfer_data);
  f.emplace_back("nft_listing_data", a.nft_listing_data);
  f.emplace_back("dex_deposit_liquidity_data", a.dex_deposit_liquidity_data);
  f.emplace_back("staking_data", a.staking_data);
  f.emplace_back("evaa_withdraw_data", a.evaa_withdraw_data);
  f.emplace_back("vesting_send_message_data", a.vesting_send_message_data);
  f.emplace_back("layerzero_packet_data", a.layerzero_packet_data);
  f.emplace_back("jvault_stake_data", a.jvault_stake_data);
  f.emplace_back("jvault_claim_data", a.jvault_claim_data);
  f.emplace_back("extra", a.extra);
  return render_value(Value::make_dict(std::move(f)));
}

// Shared CLI dump adapter over ClassifyCore (§11 item 2 / prod-design §2). The
// core computes a buffered per-trace ClassifyResult; this reproduces today's
// stdout byte-for-byte (SKIP header, `=== fixture`, ERROR/TRACE_FAIL lines, then
// per surviving action either the classify line (emit_action) or the serialized
// Action row (build_action + render_action)). --classify and --actions differ
// ONLY in the per-action render, so they share this.
int run_dump(const std::string &artifact_path, const std::vector<std::string> &paths,
             bool actions_mode) {
  auto r_ir = load_ir(artifact_path);
  if (r_ir.is_error()) {
    std::fprintf(stderr, "failed to load IR artifact: %s\n", r_ir.error().message().c_str());
    return 2;
  }
  LoadedIr ir = r_ir.move_as_ok();
  warn_missing_artifact_parsers(ir);

  ClassifySetup setup = prepare_classify(ir);
  if (setup.table_missing || setup.fn_missing) {
    std::fprintf(stderr, "%s\n", setup.error.c_str());
    return 2;
  }
  for (const auto &s : setup.skips) {
    std::printf("SKIP %s reason=%s\n", s.first.c_str(), s.second.c_str());
  }

  namespace fs = std::filesystem;
  std::vector<std::string> fixtures;
  for (const std::string &p : paths) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
      std::vector<std::string> names;
      for (const auto &e : fs::directory_iterator(p)) {
        if (e.path().extension() == ".lz4") {
          names.push_back(e.path().string());
        }
      }
      std::sort(names.begin(), names.end());
      fixtures.insert(fixtures.end(), names.begin(), names.end());
    } else {
      fixtures.push_back(p);
    }
  }

  for (const std::string &f : fixtures) {
    std::printf("=== %s\n", fs::path(f).filename().string().c_str());
    auto r_ctx = load_trace_context(f);
    if (r_ctx.is_error()) {
      std::printf("ERROR: %s\n", r_ctx.error().message().c_str());
      continue;
    }
    auto ctx = r_ctx.move_as_ok();
    // Orphan warning: stderr only (stdout is the A/B channel; never fires on corpus).
    if (ctx.tree.unlinked != 0) {
      std::fprintf(stderr, "WARN %s: %zu orphan nodes unreachable from root\n",
                   fs::path(f).filename().string().c_str(), ctx.tree.unlinked);
    }
    FixtureLookupSource src(&ctx.trace.interfaces);

    ClassifyResult res = classify_trace(ctx, ir, setup, src);
    if (res.failure) {
      // Per-trace exception boundary (event_processing.py): buffered — the
      // regular actions are discarded, a single TRACE_FAIL on stdout, the reason
      // on stderr (A/B channel stays byte-comparable). In --actions mode the
      // basic-actions fallback rows (Step 2c) follow behind a FALLBACK tag.
      // No corpus trace triggers this — the fallback path is ungated.
      std::printf("TRACE_FAIL\n");
      std::fprintf(stderr, "TRACE_FAIL %s: %s\n", fs::path(f).filename().string().c_str(),
                   res.failure_reason.c_str());
      if (actions_mode) {
        for (const CoreAction &a : res.fallback_actions) {
          Action act;
          if (!build_action(a.produced, act)) continue;
          std::printf("FALLBACK anchor=%s %s\n", block_key(a.anchor).c_str(),
                      render_action(act).c_str());
        }
      }
      continue;
    }
    for (const CoreAction &a : res.actions) {
      if (actions_mode) {
        Action act;
        if (!build_action(a.produced, act)) continue;  // btype outside ported set
        std::printf("ACTION anchor=%s %s\n", block_key(a.anchor).c_str(),
                    render_action(act).c_str());
      } else {
        emit_action(a);
      }
    }
  }
  return 0;
}

}  // namespace

int run_classify(const std::string &artifact_path, const std::vector<std::string> &paths) {
  return run_dump(artifact_path, paths, /*actions_mode=*/false);
}

int run_actions(const std::string &artifact_path, const std::vector<std::string> &paths) {
  return run_dump(artifact_path, paths, /*actions_mode=*/true);
}

}  // namespace mch
