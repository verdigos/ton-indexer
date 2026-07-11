#include "Classify.h"

#include "BlockTree.h"
#include "BuildDriver.h"
#include "BuildRuntime.h"
#include "ExprRuntime.h"
#include "GenBuilds.h"
#include "HostRegistry.h"
#include "IrLoader.h"
#include "MsgParse.h"
#include "Walker.h"

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

constexpr std::uint32_t kExcessOpcode = 0xD53276DB;
constexpr std::uint32_t kBounceOpcode = 0xFFFFFFFF;

// A produced action, COLLECTED during the matcher loop and emitted only after
// the post-process passes run (Phase E): the passes merge/remove produced
// blocks, so `data`/`consumed` must be rendered from the FINAL block state, not
// at fire time. aux/flows are fire-time (the anchor's excess/bounce, unchanged
// by pairing).
struct PendingAction {
  std::string matcher_name;
  std::string anchor_key;
  Block *produced;
  std::string aux_s;
  std::string flows_s;
};

// engine.py IrMatcher.try_build ported: returns true iff a block was produced
// (collects a PendingAction). Rejections and mismatches collect nothing.
bool try_build(const CompiledMatcher &m, BuildOutcome (*fn)(BuildEnv &), Block *block,
               BlockArena &arena, const std::set<std::string> &kinds,
               const FixtureLookupSource &src, std::vector<PendingAction> &out) {
  auto res = matcher_match(m, block);  // test_self + walk (root absorb=False)
  if (!res) {
    return false;  // structural mismatch
  }
  std::vector<Value> slots = slots_from_captures(res->captures);
  std::vector<Value> consumed_vals;
  for (Block *b : res->consumed) {
    consumed_vals.push_back(Value::make_block(b));
  }
  BuildOutcome o = run_two_phase(fn, block, slots,
                                 Value::make_list(std::move(consumed_vals)), kinds, src);
  if (o.is_rejected) {
    return false;  // clean rejection §10.7: no consumption, no aux append
  }

  Block *produced = arena.make(o.btype.empty() ? m.produces.front() : o.btype);
  produced->data = o.data;
  produced->failed = o.failed;
  produced->broken = o.broken;
  if (!produced->merge_blocks(res->consumed)) {
    return false;  // Python: merge raises -> try_build catches -> clean rejection
  }

  // §8.7 `shape` hook: LAST build step, after merge (engine.py _apply_shape).
  // The shaper receives the produced block and a match view carrying the
  // capture bindings (scalar name -> block) and the consumed set.
  if (!m.ref_shapers.empty()) {
    const std::string &shaper_name = *m.ref_shapers.begin();
    auto sit = host_shapers().find(shaper_name);
    if (sit != host_shapers().end()) {
      ShaperMatch sm;
      sm.arena = &arena;
      sm.consumed = res->consumed;
      for (const Capture &c : res->captures) {
        sm.captures[c.name] = c.vals.empty() ? nullptr : c.vals.front();
      }
      sit->second(produced, sm);
    }
  }

  // Root-level excess/bounce auto-append (engine.py try_build): from the
  // ANCHOR's next_blocks (unchanged by the merge), canonical order.
  std::vector<Block *> aux;
  {
    std::vector<Block *> nb = block->next_blocks;
    std::stable_sort(nb.begin(), nb.end(),
                     [](const Block *a, const Block *b) { return a->min_lt < b->min_lt; });
    for (Block *n : nb) {
      if (n->btype != "call_contract" || !n->opcode) {
        continue;
      }
      if ((*n->opcode == kExcessOpcode && m.include_excess) ||
          (*n->opcode == kBounceOpcode && m.include_bounces)) {
        aux.push_back(n);
      }
    }
  }

  std::string aux_s = "[";
  for (std::size_t i = 0; i < aux.size(); i++) {
    if (i) aux_s += ",";
    aux_s += block_key(aux[i]);
  }
  aux_s += "]";

  std::string flows = "[";
  for (std::size_t i = 0; i < o.flows.size(); i++) {
    if (i) flows += ",";
    flows += o.flows[i].kind + "(";
    for (std::size_t j = 0; j < o.flows[i].args.size(); j++) {
      if (j) flows += ",";
      flows += render_value(o.flows[i].args[j]);
    }
    flows += ")";
  }
  flows += "]";

  out.push_back(PendingAction{m.name, block_key(block), produced, std::move(aux_s), std::move(flows)});
  return true;
}

// --- Phase E: host post-process passes (blocks/liquidity.py) -----------------
// value_flow / contract_deployments are DROPPED from C++ scope (plan §7 banner);
// the passes only reproduce the block-tree + data effects the classify dump
// renders. All surgery is throw-safe (no aborts).

Value pp_field(const Value &d, const std::string &k) {
  const Value *v = d.field(k);
  return v != nullptr ? *v : Value::null();
}
bool pp_eq(const Value &a, const Value &b) {
  EvalResult r = rt_eq(a, b);
  return !r.faulted && r.value.t == VType::Bool && r.value.boolean;
}

// blocks/liquidity.py combine_deposits: merge the partial (first) + final
// deposit records. Throws on the consistency checks Python raises on (caller
// catches -> the pair is left unmerged, matching a Python raise's net effect of
// no merge for that pair over the corpus, which never trips these).
Value combine_deposits(const Value &first, const Value &final_d) {
  Value tf1 = pp_field(first, "target_asset_1"), tf2 = pp_field(first, "target_asset_2");
  Value tg1 = pp_field(final_d, "target_asset_1"), tg2 = pp_field(final_d, "target_asset_2");
  bool same = (pp_eq(tf1, tg1) && pp_eq(tf2, tg2)) || (pp_eq(tf1, tg2) && pp_eq(tf2, tg1));
  if (!same) throw std::runtime_error("combine_deposits: target assets mismatch");

  struct Tup { Value asset, amount, wallet; };
  std::vector<Tup> tuples;
  auto scan = [&](const Value &d, bool merge) {
    for (int i = 1; i <= 2; i++) {
      std::string si = std::to_string(i);
      Value asset = pp_field(d, "asset_" + si);
      if (asset.is_null()) continue;
      Value amount = pp_field(d, "amount_" + si);
      Value wallet = pp_field(d, "user_jetton_wallet_" + si);
      if (merge) {
        int idx = -1;
        for (std::size_t j = 0; j < tuples.size(); j++) {
          if (pp_eq(tuples[j].asset, asset)) { idx = static_cast<int>(j); break; }
        }
        if (idx >= 0) {
          if (!pp_eq(tuples[idx].wallet, wallet)) {
            throw std::runtime_error("combine_deposits: user wallet mismatch");
          }
          EvalResult s = rt_add(tuples[idx].amount, amount);
          if (s.faulted) throw std::runtime_error("combine_deposits: amount add faulted");
          tuples[idx].amount = s.value;
          if (!wallet.is_null()) tuples[idx].wallet = wallet;
          continue;
        }
      }
      tuples.push_back({asset, amount, wallet});
    }
  };
  scan(first, false);
  scan(final_d, true);

  // exactly 2 unique assets, matching the target set.
  if (tuples.size() != 2) throw std::runtime_error("combine_deposits: expected 2 unique assets");
  bool ut = (pp_eq(tuples[0].asset, tf1) && pp_eq(tuples[1].asset, tf2)) ||
            (pp_eq(tuples[0].asset, tf2) && pp_eq(tuples[1].asset, tf1));
  if (!ut) throw std::runtime_error("combine_deposits: assets don't match target");

  // result = final copy, overwrite asset/amount/wallet_{1,2} + target ordering.
  Value::Fields out;
  if (final_d.fields) out = *final_d.fields;
  auto set_field = [&](const std::string &k, Value v) {
    for (auto &kv : out) {
      if (kv.first == k) { kv.second = std::move(v); return; }
    }
    out.emplace_back(k, std::move(v));
  };
  for (int i = 0; i < 2; i++) {
    std::string si = std::to_string(i + 1);
    set_field("asset_" + si, tuples[i].asset);
    set_field("amount_" + si, tuples[i].amount);
    if (!tuples[i].wallet.is_null()) set_field("user_jetton_wallet_" + si, tuples[i].wallet);
  }
  set_field("target_asset_1", tuples[0].asset);
  set_field("target_asset_2", tuples[1].asset);
  return Value::make_dict(std::move(out));
}

// blocks/liquidity.py post_process_dedust_liquidity: pair each partial deposit
// with a final deposit sharing a deposit_contract, merge the data into the
// final, and drop the partial from the spine list.
void post_process_dedust_liquidity(std::vector<Block *> &blocks, std::set<Block *> &removed) {
  std::vector<Block *> firsts, finals;
  std::map<std::string, int> used;
  auto contract_key = [](const Block *b) {
    Value c = pp_field(b->data, "deposit_contract");
    return c.t == VType::Account ? (c.addr_none ? std::string("addr_none") : c.str) : c.describe();
  };
  for (Block *b : blocks) {
    if (b->btype == "dedust_deposit_liquidity_partial") {
      firsts.push_back(b);
      used[contract_key(b)]++;
    } else if (b->btype == "dedust_deposit_liquidity") {
      finals.push_back(b);
      used[contract_key(b)]++;
    }
  }
  for (const auto &kv : used) {
    if (kv.second > 2) return;  // logger.warning + skip merging
  }
  for (Block *first : firsts) {
    Block *final_b = nullptr;
    for (Block *b : finals) {
      if (contract_key(b) == contract_key(first)) { final_b = b; break; }
    }
    if (final_b == nullptr) continue;
    Value merged;
    try {
      merged = combine_deposits(first->data, final_b->data);
    } catch (const std::exception &) {
      continue;  // Python raise -> pair unmerged (unreached over corpus)
    }
    blocks.erase(std::remove(blocks.begin(), blocks.end(), first), blocks.end());
    removed.insert(first);
    for (const EventNode *n : first->event_nodes) final_b->event_nodes.push_back(n);
    final_b->data = std::move(merged);
    for (Block *cb : first->children_blocks) final_b->children_blocks.push_back(cb);
    // dedup children_blocks (Python list(set(...)); order washed out by the
    // sorted `consumed` rendering).
    std::sort(final_b->children_blocks.begin(), final_b->children_blocks.end());
    final_b->children_blocks.erase(
        std::unique(final_b->children_blocks.begin(), final_b->children_blocks.end()),
        final_b->children_blocks.end());
    final_b->calculate_min_max_lt();
  }
}

// blocks/liquidity.py post_process_stonfi_v2_liquidity (R08 pairing pass). ZERO
// corpus coverage (no StonfiV2ProvideLiquidity messages in the fixtures) — the
// `< 2 legs` guard returns immediately for every corpus trace. Ported faithfully
// for future coverage; value_flow / contract_deployments / initiating_event_node
// are out of C++ scope (calculate_min_max_lt recomputes lt from the tree).
bool is_stonfi_v2_leg(const Block *b) {
  if (b->btype != "dex_deposit_liquidity") return false;
  Value dex = pp_field(b->data, "dex");
  return dex.t == VType::Str && dex.str == "stonfi_v2";
}
std::pair<int, std::int64_t> stonfi_v2_fire_order(const Block *b) {
  const Block *anchor = nullptr;
  for (Block *c : b->children_blocks) {
    if (c->opcode && *c->opcode == 0x37c096dfu) { anchor = c; break; }
  }
  int depth = 0;
  for (const Block *cur = anchor; cur != nullptr && cur->previous_block != nullptr;
       cur = cur->previous_block) {
    depth++;
  }
  return {depth, b->min_lt};
}
// The consumed in-leg head: the unique child whose previous_block is `proxied`.
Block *stonfi_v2_leg_head(Block *block, Block *proxied) {
  for (Block *c : block->children_blocks) {
    if (c->previous_block == proxied) return c;
  }
  return nullptr;
}
void stonfi_v2_data_merge(Block *final_b, Block *partial) {
  auto set = [&](const std::string &k, Value v) {
    if (!final_b->data.fields) return;
    for (auto &kv : *final_b->data.fields) {
      if (kv.first == k) { kv.second = std::move(v); return; }
    }
    final_b->data.fields->emplace_back(k, std::move(v));
  };
  set("amount_2", pp_field(partial->data, "amount_1"));
  set("asset_2", pp_field(partial->data, "asset_1"));
  set("sender_wallet_2", pp_field(partial->data, "sender_wallet_1"));
  if (pp_field(final_b->data, "lp_tokens_minted").is_null()) {
    set("lp_tokens_minted", pp_field(partial->data, "lp_tokens_minted"));
  }
}
void post_process_stonfi_v2_liquidity(std::vector<Block *> &blocks, BlockArena &arena,
                                      std::set<Block *> &removed) {
  std::vector<Block *> deposits;
  for (Block *b : blocks) {
    if (is_stonfi_v2_leg(b)) deposits.push_back(b);
  }
  if (deposits.size() < 2) return;  // corpus always returns here
  std::stable_sort(deposits.begin(), deposits.end(), [](const Block *a, const Block *b) {
    return stonfi_v2_fire_order(a) < stonfi_v2_fire_order(b);
  });
  std::vector<Block *> alive;
  for (Block *b : deposits) {
    Block *a = nullptr;
    for (Block *cand : alive) {
      if (cand->previous_block != nullptr && cand->previous_block == b->previous_block) a = cand;
    }
    if (a != nullptr) {
      Value as = pp_field(a->data, "sender"), bs = pp_field(b->data, "sender");
      Value ap = pp_field(a->data, "pool"), bp = pp_field(b->data, "pool");
      Value al = pp_field(a->data, "lp_tokens_minted"), bl = pp_field(b->data, "lp_tokens_minted");
      Block *proxied = a->previous_block;
      if (pp_eq(as, bs) && pp_eq(ap, bp) && !pp_eq(al, bl)) {
        // _stonfi_v2_merge_paired: EmptyBlock proxy between the shared parent and
        // [partial, in_transfer], then absorb the partial + proxy into `final`.
        Block *in_transfer = stonfi_v2_leg_head(b, proxied);
        Block *proxy = arena.make("empty");
        if (proxied != nullptr && in_transfer != nullptr) {
          proxied->insert_between({a, in_transfer}, proxy);
        }
        b->children_blocks.push_back(a);
        b->children_blocks.push_back(proxy);
        a->parent = b;
        proxy->parent = b;
        for (const EventNode *n : a->event_nodes) b->event_nodes.push_back(n);
        b->calculate_min_max_lt();
        stonfi_v2_data_merge(b, a);
      } else {
        // _stonfi_v2_absorb_unpaired: absorb sibling + shared parent, no data.
        b->children_blocks.push_back(a);
        if (proxied != nullptr) b->children_blocks.push_back(proxied);
        a->parent = b;
        if (proxied != nullptr) proxied->parent = b;
        for (const EventNode *n : a->event_nodes) b->event_nodes.push_back(n);
        b->calculate_min_max_lt();
      }
      blocks.erase(std::remove(blocks.begin(), blocks.end(), a), blocks.end());
      alive.erase(std::remove(alive.begin(), alive.end(), a), alive.end());
      removed.insert(a);
    }
    alive.push_back(b);
  }
}

// Render + print a collected action from its FINAL (post-pass) state.
void emit_action(const PendingAction &a) {
  std::vector<std::string> ck;
  for (Block *cb : a.produced->children_blocks) ck.push_back(block_key(cb));
  std::sort(ck.begin(), ck.end());
  std::string consumed_s = "[";
  for (std::size_t i = 0; i < ck.size(); i++) {
    if (i) consumed_s += ",";
    consumed_s += ck[i];
  }
  consumed_s += "]";
  std::printf("ACTION %s anchor=%s btype=%s failed=%d broken=%d data=%s flows=%s "
              "consumed=%s auto=%s\n",
              a.matcher_name.c_str(), a.anchor_key.c_str(), a.produced->btype.c_str(),
              a.produced->failed ? 1 : 0, a.produced->broken ? 1 : 0,
              render_value(a.produced->data).c_str(), a.flows_s.c_str(), consumed_s.c_str(),
              a.aux_s.c_str());
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

// Dispatch a produced block to its fill (block_to_action match). Returns false
// for a btype outside the ported set (skip-table entry).
bool fill_action(Block *b, Action &a) {
  const Value &d = b->data;
  const std::string &t = b->btype;
  if (t == "jetton_transfer") fill_jetton_transfer(d, a);
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
  a.tx_hashes.assign(ths.begin(), ths.end());
  a.start_lt = b->min_lt;
  a.end_lt = b->event_nodes.empty() ? 0 : maxlt;
  a.start_utime = b->event_nodes.empty() ? 0 : minu;
  a.end_utime = b->event_nodes.empty() ? 0 : maxu;
  a.mc_seqno_end = b->event_nodes.empty() ? 0 : seqmax;
  return fill_action(b, a);
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

}  // namespace

int run_classify(const std::string &artifact_path, const std::vector<std::string> &paths) {
  auto r_ir = load_ir(artifact_path);
  if (r_ir.is_error()) {
    std::fprintf(stderr, "failed to load IR artifact: %s\n", r_ir.error().message().c_str());
    return 2;
  }
  LoadedIr ir = r_ir.move_as_ok();
  warn_missing_artifact_parsers(ir);

  const char *table_kind = nullptr;
  const std::vector<GenBuild> *table = select_gen_builds(ir.source_sha256, &table_kind);
  if (table == nullptr) {
    std::fprintf(stderr,
                 "no generated build table matches this artifact (loaded sha %s; ir table %s, "
                 "testbed table %s) — the CMake build pairs tables with ir/mch_ir.json and the "
                 "optional -DMCH_TESTBED_IR artifact; rebuild ton-mch-engine or pass a paired "
                 "artifact\n",
                 ir.source_sha256.c_str(), gen_builds_ir_source_sha(),
                 *gen_builds_testbed_source_sha() ? gen_builds_testbed_source_sha() : "(stub)");
    return 2;
  }

  std::vector<int> included;
  std::vector<std::pair<std::string, std::string>> skips;
  std::set<std::string> kinds;
  for (int i = 0; i < static_cast<int>(ir.matchers.size()); i++) {
    std::string reason = build_skip_reason(ir.matchers[i]);
    if (!reason.empty()) {
      skips.emplace_back(ir.matchers[i].name, reason);
      continue;
    }
    included.push_back(i);
    kinds.insert(ir.matchers[i].ref_lookups.begin(), ir.matchers[i].ref_lookups.end());
  }
  std::sort(skips.begin(), skips.end());
  for (const auto &s : skips) {
    std::printf("SKIP %s reason=%s\n", s.first.c_str(), s.second.c_str());
  }

  std::unordered_map<int, BuildOutcome (*)(BuildEnv &)> build_fns;
  for (const GenBuild &g : *table) {
    build_fns.emplace(g.id, g.fn);
  }
  for (int mi : included) {
    if (build_fns.find(ir.matchers[mi].artifact_index) == build_fns.end()) {
      std::fprintf(stderr,
                   "generated %s build table has no fn for matcher '%s' (artifact index %d) — "
                   "rebuild ton-mch-engine (CMake regenerates the paired tables)\n",
                   table_kind, ir.matchers[mi].name.c_str(), ir.matchers[mi].artifact_index);
      return 2;
    }
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
    // Python to_tree asserts a single root; the C++ substrate tolerates orphan
    // subtrees but the loss must not be silent (BlockTree.cpp). Surface to
    // stderr only — stdout (the A/B channel) stays byte-identical, and no
    // corpus trace has orphans, so this never fires in the gates.
    if (ctx.tree.unlinked != 0) {
      std::fprintf(stderr, "WARN %s: %zu orphan nodes unreachable from root\n",
                   fs::path(f).filename().string().c_str(), ctx.tree.unlinked);
    }
    FixtureLookupSource src(&ctx.trace.interfaces);

    // Synthetic root wrapper (process_event_async: Block('root').connect(...)).
    Block *wrapper = ctx.arena.make("root");
    wrapper->connect(ctx.root);

    // Per-trace exception boundary (Python parity): a pred/builder/parser
    // exception escaping match/build — a recursion-depth trip (Walker
    // MatchDepthExceeded), a two-phase non-convergence (run_two_phase), a
    // pytoniq BOC/index fault, a VmError on a pruned ref — propagates OUT of
    // the engine to event_processing.py's per-trace `except Exception`, which
    // discards the trace's actions and falls back. Here that is a single
    // `TRACE_FAIL` line on stdout (the reason goes to stderr so the A/B
    // channel is byte-comparable). No corpus trace triggers this.
    std::vector<PendingAction> pending;
    try {
      for (int mi : included) {
        const CompiledMatcher &m = ir.matchers[mi];
        auto fn = build_fns.at(m.artifact_index);
        // Python bfs_iter port: FIFO; cur's next_blocks are read AFTER
        // try_build ran on cur (generator resume order) — merges mutate the
        // tree mid-walk.
        std::deque<Block *> queue{wrapper};
        while (!queue.empty()) {
          Block *cur = queue.front();
          queue.pop_front();
          if (cur->parent == nullptr) {
            try_build(m, fn, cur, ctx.arena, kinds, src, pending);
          }
          for (Block *n : cur->next_blocks) {
            queue.push_back(n);
          }
        }
      }

      // Phase E: post-process passes run AFTER the matcher loop, over the flat
      // spine list ([root] + bfs), then the SURVIVING produced actions are
      // emitted from their final state (event_processing.py
      // process_event_async_with_postprocessing).
      std::vector<Block *> spine;
      {
        std::deque<Block *> q{wrapper};
        while (!q.empty()) {
          Block *cur = q.front();
          q.pop_front();
          spine.push_back(cur);
          for (Block *n : cur->next_blocks) q.push_back(n);
        }
      }
      // The passes SUPPRESS only the produced blocks they merge away (a paired
      // partial); every other collected action is still emitted (the dump is
      // per-fire — a block later consumed by another matcher keeps its line).
      std::set<Block *> removed;
      post_process_dedust_liquidity(spine, removed);
      post_process_stonfi_v2_liquidity(spine, ctx.arena, removed);
      for (const PendingAction &a : pending) {
        if (!removed.count(a.produced)) emit_action(a);
      }
    } catch (const std::exception &e) {
      std::printf("TRACE_FAIL\n");
      std::fprintf(stderr, "TRACE_FAIL %s: %s\n",
                   fs::path(f).filename().string().c_str(), e.what());
    } catch (...) {
      std::printf("TRACE_FAIL\n");
      std::fprintf(stderr, "TRACE_FAIL %s: unknown exception\n",
                   fs::path(f).filename().string().c_str());
    }
  }
  return 0;
}

// Phase E part 2 dump (`--actions`): identical collect->pass pipeline as
// run_classify, but each surviving post-pass produced block is rendered as a
// serialized Action row (build_action + render_action). Deliberately mirrors
// run_classify's loop verbatim (shared try_build / post_process helpers) so the
// two dumps stay structurally in lock-step; only the emit step differs.
int run_actions(const std::string &artifact_path, const std::vector<std::string> &paths) {
  auto r_ir = load_ir(artifact_path);
  if (r_ir.is_error()) {
    std::fprintf(stderr, "failed to load IR artifact: %s\n", r_ir.error().message().c_str());
    return 2;
  }
  LoadedIr ir = r_ir.move_as_ok();
  warn_missing_artifact_parsers(ir);

  const char *table_kind = nullptr;
  const std::vector<GenBuild> *table = select_gen_builds(ir.source_sha256, &table_kind);
  if (table == nullptr) {
    std::fprintf(stderr, "no generated build table matches this artifact (loaded sha %s)\n",
                 ir.source_sha256.c_str());
    return 2;
  }

  std::vector<int> included;
  std::vector<std::pair<std::string, std::string>> skips;
  std::set<std::string> kinds;
  for (int i = 0; i < static_cast<int>(ir.matchers.size()); i++) {
    std::string reason = build_skip_reason(ir.matchers[i]);
    if (!reason.empty()) {
      skips.emplace_back(ir.matchers[i].name, reason);
      continue;
    }
    included.push_back(i);
    kinds.insert(ir.matchers[i].ref_lookups.begin(), ir.matchers[i].ref_lookups.end());
  }
  std::sort(skips.begin(), skips.end());
  for (const auto &s : skips) {
    std::printf("SKIP %s reason=%s\n", s.first.c_str(), s.second.c_str());
  }

  std::unordered_map<int, BuildOutcome (*)(BuildEnv &)> build_fns;
  for (const GenBuild &g : *table) {
    build_fns.emplace(g.id, g.fn);
  }
  for (int mi : included) {
    if (build_fns.find(ir.matchers[mi].artifact_index) == build_fns.end()) {
      std::fprintf(stderr, "generated %s build table has no fn for matcher '%s' (index %d)\n",
                   table_kind, ir.matchers[mi].name.c_str(), ir.matchers[mi].artifact_index);
      return 2;
    }
  }

  namespace fs = std::filesystem;
  std::vector<std::string> fixtures;
  for (const std::string &p : paths) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
      std::vector<std::string> names;
      for (const auto &e : fs::directory_iterator(p)) {
        if (e.path().extension() == ".lz4") names.push_back(e.path().string());
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
    if (ctx.tree.unlinked != 0) {
      std::fprintf(stderr, "WARN %s: %zu orphan nodes unreachable from root\n",
                   fs::path(f).filename().string().c_str(), ctx.tree.unlinked);
    }
    FixtureLookupSource src(&ctx.trace.interfaces);
    Block *wrapper = ctx.arena.make("root");
    wrapper->connect(ctx.root);

    std::vector<PendingAction> pending;
    try {
      for (int mi : included) {
        const CompiledMatcher &m = ir.matchers[mi];
        auto fn = build_fns.at(m.artifact_index);
        std::deque<Block *> queue{wrapper};
        while (!queue.empty()) {
          Block *cur = queue.front();
          queue.pop_front();
          if (cur->parent == nullptr) {
            try_build(m, fn, cur, ctx.arena, kinds, src, pending);
          }
          for (Block *n : cur->next_blocks) queue.push_back(n);
        }
      }
      std::vector<Block *> spine;
      {
        std::deque<Block *> q{wrapper};
        while (!q.empty()) {
          Block *cur = q.front();
          q.pop_front();
          spine.push_back(cur);
          for (Block *n : cur->next_blocks) q.push_back(n);
        }
      }
      std::set<Block *> removed;
      post_process_dedust_liquidity(spine, removed);
      post_process_stonfi_v2_liquidity(spine, ctx.arena, removed);
      for (const PendingAction &p : pending) {
        if (removed.count(p.produced)) continue;
        Action act;
        if (!build_action(p.produced, act)) continue;  // btype outside ported set
        std::printf("ACTION anchor=%s %s\n", p.anchor_key.c_str(), render_action(act).c_str());
      }
    } catch (const std::exception &e) {
      std::printf("TRACE_FAIL\n");
      std::fprintf(stderr, "TRACE_FAIL %s: %s\n", fs::path(f).filename().string().c_str(), e.what());
    } catch (...) {
      std::printf("TRACE_FAIL\n");
      std::fprintf(stderr, "TRACE_FAIL %s: unknown exception\n",
                   fs::path(f).filename().string().c_str());
    }
  }
  return 0;
}

}  // namespace mch
