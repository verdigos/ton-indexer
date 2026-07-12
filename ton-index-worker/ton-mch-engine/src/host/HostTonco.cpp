// Tonco liquidity-deposit + swap host fns (builders/tonco.py, builders/tonco_swap.py
// + blocks/liquidity.py build_tonco_deposit_liquidity_core / blocks/swaps.py
// build_tonco_swap_core). See host/HostImpls.h for the internal registry surface.
#include "host/HostImpls.h"

#include "host/DexPton.h"
#include "host/HostCommon.h"

#include "BlockTree.h"
#include "BuildRuntime.h"
#include "MsgParse.h"
#include "parse/PSlice.h"

#include "common/refint.h"
#include "vm/cellslice.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mch {

namespace {

constexpr std::uint32_t kJettonNotify = 0x7362d09c;
constexpr std::uint32_t kFundAccountPayload = 0x4468de77;
constexpr std::uint32_t kV3Swap = 0xa7fb58f8;         // POOLV3_SWAP (swap request + in-payload)
constexpr std::uint32_t kV3PayTo = 0xa1daa96d;        // ROUTERV3_PAY_TO
constexpr std::uint32_t kJettonTransfer = 0x0f8a7ea5;
constexpr std::uint32_t kPTonTransfer = 0x01f3835d;

td::Result<vm::CellSlice> open_ref_t(const td::Ref<vm::Cell> &c) {
  bool special = false;
  try {
    return vm::load_cell_slice_special(c, special);
  } catch (...) {
    return td::Status::Error("ref slice");
  }
}

Value amount_or_zero_t(td::RefInt256 v) {
  return v.is_null() ? Value::make_amount(td::make_refint(0)) : Value::make_amount(std::move(v));
}

const Block *find_call_t(const std::vector<Block *> &blocks, std::uint32_t op) {
  for (const Block *b : blocks) {
    if (is_call_op(b, op)) {
      return b;
    }
  }
  return nullptr;
}

// get_jetton_wallet(addr) -> Asset (pton master -> TON), or Null when the
// wallet lookup misses (caller decides fallback).
Value wallet_asset(BuildEnv &env, const std::string &addr) {
  Value jw = env.lookups->get("jetton_wallet", std::vector<Value>{Value::make_str(addr)});
  return wallet_jetton_asset(jw, /*pton_conversion=*/true);
}

std::optional<std::string> acc_str_t(const Value &v) {
  if (v.t == VType::Account && !v.addr_none) {
    return v.str;
  }
  return std::nullopt;
}

// messages/swaps.py ToncoPoolV3Swap: the core reads only source_wallet (the
// two ref cells carry amounts/payloads the core does not touch here).
td::Result<Value> parse_v3_swap_source_wallet(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64) || !cs.advance(32 + 64)) {
    return td::Status::Error("v3 swap: header underflow");
  }
  TRY_RESULT(owner, load_address_py(cs));
  (void)owner;
  return load_address_py(cs);  // source_wallet
}

// messages/swaps.py ToncoRouterV3PayTo: exit_code + the coinsinfo maybe-ref
// (amount0/jetton0/amount1/jetton1). The conditional swap/burn info cells are
// unused by the core.
struct RouterPayTo {
  std::uint32_t exit_code{0};
  td::RefInt256 amount0, amount1;  // null == None
  Value jetton0, jetton1;          // account or Null
};
td::Result<RouterPayTo> parse_v3_pay_to(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64) || !cs.advance(32 + 64)) {
    return td::Status::Error("v3 pay_to: header underflow");
  }
  TRY_RESULT(r0, load_address_py(cs));
  (void)r0;
  TRY_RESULT(r1, load_address_py(cs));
  (void)r1;
  if (!cs.have(32 + 64)) {
    return td::Status::Error("v3 pay_to: exit/seqno underflow");
  }
  RouterPayTo pt;
  pt.exit_code = static_cast<std::uint32_t>(cs.fetch_ulong(32));
  cs.advance(64);  // seqno
  if (!cs.have(1)) {
    return td::Status::Error("v3 pay_to: coinsinfo maybe bit");
  }
  if (cs.fetch_ulong(1)) {
    if (cs.size_refs() < 1) {
      return td::Status::Error("v3 pay_to: coinsinfo ref missing");
    }
    TRY_RESULT(ci, open_ref_t(cs.fetch_ref()));
    TRY_RESULT_ASSIGN(pt.amount0, load_coins_py(ci));
    TRY_RESULT_ASSIGN(pt.jetton0, load_address_py(ci));
    TRY_RESULT_ASSIGN(pt.amount1, load_coins_py(ci));
    TRY_RESULT_ASSIGN(pt.jetton1, load_address_py(ci));
  }
  return pt;
}

// messages/swaps.py ToncoPoolV3SwapPayload.get_target_wallets_and_amounts_
// recursive: walk the nested in-transfer payload cells collecting
// (target_wallet, min_out) pairs. Depth-guarded (malformed/adversarial nesting
// must not unbound-recurse); a bad inner tail is swallowed (Python try/except).
constexpr int kMaxPayloadDepth = 64;
void collect_payload_targets(vm::CellSlice cs, std::vector<std::pair<std::string, td::RefInt256>> &out,
                             int depth) {
  if (depth > kMaxPayloadDepth) {
    return;
  }
  if (!cs.have(32) || !cs.advance(32)) {
    return;  // opcode
  }
  auto r_addr = load_address_py(cs);
  if (r_addr.is_error()) {
    return;
  }
  Value target = r_addr.move_as_ok();
  if (!cs.have(160)) {
    return;
  }
  cs.advance(160);  // price_limit_sqrt
  auto r_min = load_coins_py(cs);
  if (r_min.is_error()) {
    return;
  }
  td::RefInt256 min_out = r_min.move_as_ok();
  if (auto ts = acc_str_t(target)) {
    out.emplace_back(*ts, min_out.is_null() ? td::make_refint(0) : min_out);
  }
  auto r_rcpt = load_address_py(cs);  // recipient
  if (r_rcpt.is_error()) {
    return;
  }
  if (!cs.have(1)) {
    return;
  }
  if (cs.fetch_ulong(1) && cs.size_refs() > 0) {  // payload maybe-ref
    try {
      auto inner = open_ref_t(cs.fetch_ref());
      if (inner.is_ok()) {
        vm::CellSlice ics = inner.move_as_ok();
        if (ics.have(32) && ics.prefetch_ulong(32) == kV3Swap) {
          collect_payload_targets(ics, out, depth + 1);
        }
      }
    } catch (...) {
      // Python swallows inner-payload errors.
    }
  }
}

}  // namespace

// builders/tonco.py _tonco_deposit_liquidity_data -> blocks/liquidity.py
// build_tonco_deposit_liquidity_core. Python wraps the whole body in try/except
// returning None, so any parse/nav failure is a Null reject here. Deterministic
// (input transfer read via btype/children_blocks/data, not event_nodes[0]).
EvalResult tonco_deposit_liquidity_data(BuildEnv &env, const std::vector<Value> &args) {
  if (args.size() != 1 || args[0].t != VType::List || args[0].items->empty()) {
    return rt_ok(Value::null());
  }
  std::vector<const Block *> consumed;
  for (const Value &v : *args[0].items) {
    const Block *b = as_block(v);
    if (b != nullptr) {
      consumed.push_back(b);
    }
  }
  if (consumed.empty()) {
    return rt_ok(Value::null());
  }
  const Block *block = consumed.front();

  // anchor <- FundAccount <- input transfer.
  const Block *p1 = block->previous_block;
  const Block *input_transfer = p1 != nullptr ? p1->previous_block : nullptr;

  const Block *nft_mint = nullptr;
  for (const Block *b : consumed) {
    if (b->btype == "nft_mint") {
      nft_mint = b;
      break;
    }
  }
  // (excess_transfers are gathered below by btype near the record assembly.)

  // add_liquidity body.
  auto r_body = block_body(block);
  if (r_body.is_error()) {
    return rt_ok(Value::null());
  }
  auto r_ctx = open_body(r_body.move_as_ok());
  if (r_ctx.is_error()) {
    return rt_ok(Value::null());
  }
  auto add_ctx = r_ctx.move_as_ok();
  auto &acs = add_ctx.cs;
  if (!acs.have(32 + 64) || !acs.advance(32 + 64)) {
    return rt_ok(Value::null());
  }
  auto r_na0 = load_coins_py(acs);  // new_amount0
  auto r_na1 = load_coins_py(acs);  // new_amount1
  auto r_ne0 = load_coins_py(acs);  // new_enough0
  auto r_ne1 = load_coins_py(acs);  // new_enough1
  if (r_na0.is_error() || r_na1.is_error() || r_ne0.is_error() || r_ne1.is_error()) {
    return rt_ok(Value::null());
  }
  td::RefInt256 new_amount0 = r_na0.move_as_ok();
  (void)r_na1;
  td::RefInt256 new_enough0 = r_ne0.move_as_ok();
  td::RefInt256 new_enough1 = r_ne1.move_as_ok();
  if (!acs.have(128 + 24 + 24)) {
    return rt_ok(Value::null());
  }
  acs.advance(128);  // liquidity
  std::int64_t tick_lower = acs.fetch_long(24);
  std::int64_t tick_upper = acs.fetch_long(24);
  bool is_first = !new_amount0.is_null() && new_amount0->sgn() > 0;

  // nft position init.
  Value lp_tokens_minted = Value::null();
  Value nft_index = Value::null();
  Value nft_address = Value::null();
  if (nft_mint != nullptr) {
    auto rb = block_body(nft_mint);
    if (rb.is_error()) {
      return rt_ok(Value::null());
    }
    auto rc = open_body(rb.move_as_ok());
    if (rc.is_error()) {
      return rt_ok(Value::null());
    }
    auto pc = rc.move_as_ok();
    auto &pcs = pc.cs;
    // opcode(32) query(64) user(addr) liquidity(128) tick_lower(24) tick_upper(24)
    if (!pcs.have(32 + 64) || !pcs.advance(32 + 64)) {
      return rt_ok(Value::null());
    }
    if (load_address_py(pcs).is_error()) {
      return rt_ok(Value::null());
    }
    if (!pcs.have(128 + 24 + 24)) {
      return rt_ok(Value::null());
    }
    td::RefInt256 liquidity = pcs.fetch_int256(128, false);
    pcs.advance(24 + 24);  // tick_lower + tick_upper
    lp_tokens_minted = Value::make_amount(std::move(liquidity));
    // nft_index lives in the old_fee_cell ref at bit offset 256+256, uint64.
    if (pcs.size_refs() < 1) {
      return rt_ok(Value::null());
    }
    auto rf = open_ref_t(pcs.fetch_ref());
    if (rf.is_error()) {
      return rt_ok(Value::null());
    }
    auto fcs = rf.move_as_ok();
    if (!fcs.have(256 + 256 + 64) || !fcs.advance(256 + 256)) {
      return rt_ok(Value::null());
    }
    nft_index = Value::make_int(refint_u64(fcs.fetch_ulong(64)));
    const Message *nm = block_msg(nft_mint);
    nft_address = account_from_opt(nm != nullptr ? nm->destination : std::nullopt);
  }

  if (input_transfer == nullptr) {
    return rt_ok(Value::null());
  }
  const Block *jetton_notify_block = nullptr;
  Value sender_wallet = Value::null();
  if (input_transfer->btype == "jetton_transfer") {
    jetton_notify_block = find_call_t(input_transfer->children_blocks, kJettonNotify);
    sender_wallet = data_field(input_transfer, "sender_wallet");
  } else if (is_call_op(input_transfer, kJettonNotify)) {
    jetton_notify_block = input_transfer;
  }
  if (jetton_notify_block == nullptr) {
    return rt_ok(Value::null());
  }

  auto rnb = block_body(jetton_notify_block);
  if (rnb.is_error()) {
    return rt_ok(Value::null());
  }
  auto r_notif = parse_message_body("JettonNotify", rnb.ok());
  if (r_notif.is_error()) {
    return rt_ok(Value::null());
  }
  Value notif = r_notif.move_as_ok();
  const Value *ja = notif.field("jetton_amount");
  const Value *fu = notif.field("from_user");
  const Value *fp = notif.field("forward_payload_cell");
  Value sent_amount = ja != nullptr ? to_amount(*ja) : Value::make_amount_none();
  Value sender = fu != nullptr ? *fu : Value::make_account_none();
  if (fp == nullptr || fp->t != VType::Cell) {
    return rt_ok(Value::null());
  }
  auto rp = open_ref_t(fp->cell);
  if (rp.is_error()) {
    return rt_ok(Value::null());
  }
  auto pcs = rp.move_as_ok();
  if (!pcs.have(32)) {
    return rt_ok(Value::null());
  }
  if (static_cast<std::uint32_t>(pcs.prefetch_ulong(32)) != kFundAccountPayload) {
    return rt_ok(Value::null());
  }
  pcs.advance(32);  // opcode
  auto r_other = load_address_py(pcs);
  if (r_other.is_error()) {
    return rt_ok(Value::null());
  }
  Value other = r_other.move_as_ok();
  // get_other_jetton_wallet(): raises if not an Address -> reject.
  if (other.t != VType::Account || other.addr_none) {
    return rt_ok(Value::null());
  }
  std::string other_wallet = other.str;

  const Message *jnm = block_msg(jetton_notify_block);
  if (jnm == nullptr || !jnm->source) {
    return rt_ok(Value::null());
  }
  std::string router_wallet = *jnm->source;

  // first asset (router wallet): null lookup -> fallback TON (NOT reject).
  Value first_asset = wallet_asset(env, router_wallet);
  if (first_asset.is_null()) {
    first_asset = Value::make_asset_ton();
  }
  // second asset (other wallet): null lookup -> stays None.
  Value second_asset = wallet_asset(env, other_wallet);

  Value amount_1, asset_1, amount_2, asset_2, sw1, sw2;
  if (is_first) {
    amount_1 = sent_amount;
    asset_1 = first_asset;
    amount_2 = Value::null();
    asset_2 = second_asset;
    sw1 = sender_wallet;
    sw2 = Value::null();
  } else {
    amount_1 = Value::null();
    asset_1 = second_asset;
    amount_2 = sent_amount;
    asset_2 = first_asset;
    sw1 = Value::null();
    sw2 = sender_wallet;
  }

  // excesses (§11.5 dual dispatch): the up-to-two jetton_transfer children of
  // the ROUTERV3_PAY_TO, each (asset, amount) — a pTON child makes it a TON
  // excess, else the leg's `.data`. No corpus coverage; ported for parity.
  std::vector<Value> excesses;
  {
    const Block *pay_to = nullptr;
    for (const Block *b : consumed) {
      if (is_call_op(b, kV3PayTo)) { pay_to = b; break; }
    }
    if (pay_to != nullptr) {
      int n = 0;
      for (const Block *b : consumed) {
        if (n >= 2) break;
        if (b->btype != "jetton_transfer" || b->previous_block != pay_to) continue;
        n++;
        const Block *pton = find_call_t(b->next_blocks, kPTonTransfer);
        Value ex_amount, ex_asset;
        if (pton != nullptr) {
          td::RefInt256 ta;
          auto rb = block_body(pton);
          if (rb.is_ok()) {
            auto rp = parse_message_body("PTonTransfer", rb.ok());
            if (rp.is_ok()) {
              const Value *tav = rp.ok().field("ton_amount");
              if (tav != nullptr && (tav->t == VType::Int || tav->t == VType::Amount)) ta = tav->num;
            }
          }
          ex_amount = amount_or_zero_t(ta);
          ex_asset = Value::make_asset_ton();
        } else {
          ex_amount = data_field(b, "amount");
          ex_asset = data_field(b, "asset");
        }
        excesses.push_back(Value::make_list(std::vector<Value>{ex_asset, ex_amount}));
      }
    }
  }

  const Message *bm = block_msg(block);
  Value::Fields d;
  d.emplace_back("sender", std::move(sender));
  d.emplace_back("pool", account_from_opt(bm != nullptr ? bm->source : std::nullopt));
  d.emplace_back("account_contract",
                 account_from_opt(bm != nullptr ? bm->destination : std::nullopt));
  d.emplace_back("position_amount_1", amount_or_zero_t(new_enough0));
  d.emplace_back("position_amount_2", amount_or_zero_t(new_enough1));
  d.emplace_back("lp_tokens_minted", std::move(lp_tokens_minted));
  d.emplace_back("tick_lower", Value::make_int64(tick_lower));
  d.emplace_back("tick_upper", Value::make_int64(tick_upper));
  d.emplace_back("nft_index", std::move(nft_index));
  d.emplace_back("nft_address", std::move(nft_address));
  d.emplace_back("amount_1", std::move(amount_1));
  d.emplace_back("asset_1", std::move(asset_1));
  d.emplace_back("sender_wallet_1", std::move(sw1));
  d.emplace_back("amount_2", std::move(amount_2));
  d.emplace_back("asset_2", std::move(asset_2));
  d.emplace_back("sender_wallet_2", std::move(sw2));
  d.emplace_back("excesses", Value::make_list(std::move(excesses)));
  return rt_ok(Value::make_obj(std::move(d)));
}

namespace {

// A jetton in/out leg's asset with the tonco pton conversion: a jetton master
// in the pton set becomes TON; otherwise the block's `.data['asset']` is kept.
Value tonco_leg_asset(const Value &asset_v) {
  if (asset_v.t == VType::Asset && asset_v.has_jetton && is_pton_master(asset_v.str)) {
    return Value::make_asset_ton();
  }
  return asset_v;
}

// builders/tonco_swap.py _derive_parts: reconstruct the legacy label roles from
// the consumed set (cyclic bodies bind no captures).
struct ToncoParts {
  const Block *anchor{nullptr};
  const Block *in_transfer{nullptr};
  std::vector<const Block *> peer_swaps;  // non-anchor swaps, lt-sorted
  std::vector<const Block *> payouts;     // lt-sorted
  std::vector<const Block *> out_transfers;
};
ToncoParts derive_tonco_parts(const std::vector<const Block *> &consumed) {
  ToncoParts p;
  p.anchor = consumed.front();
  auto by_lt = [](const Block *a, const Block *b) { return a->min_lt < b->min_lt; };
  std::vector<const Block *> swaps;
  std::set<const Block *> swap_ids;
  for (const Block *b : consumed) {
    if (is_call_op(b, kV3Swap)) {
      swaps.push_back(b);
      swap_ids.insert(b);
    }
  }
  for (const Block *b : consumed) {
    if (is_call_op(b, kV3PayTo)) p.payouts.push_back(b);
  }
  std::stable_sort(p.payouts.begin(), p.payouts.end(), by_lt);
  std::set<const Block *> payout_ids(p.payouts.begin(), p.payouts.end());
  for (const Block *b : swaps) {
    if (b != p.anchor) p.peer_swaps.push_back(b);
  }
  std::stable_sort(p.peer_swaps.begin(), p.peer_swaps.end(), by_lt);
  const Block *prev = p.anchor->previous_block;
  if (prev != nullptr) {
    p.in_transfer = (prev->btype == "jetton_transfer") ? prev : prev->previous_block;
  }
  auto leads_to_hop = [&](const Block *b) -> bool {
    for (Block *c : b->next_blocks) {
      if (swap_ids.count(c)) return true;
      if (is_call_op(c, kPTonTransfer)) {
        for (Block *cc : c->next_blocks) {
          if (is_call_op(cc, kJettonNotify)) {
            for (Block *g : cc->next_blocks) {
              if (swap_ids.count(g)) return true;
            }
          }
        }
      }
    }
    return false;
  };
  for (const Block *b : consumed) {
    if (b->previous_block != nullptr && payout_ids.count(b->previous_block) &&
        (b->btype == "jetton_transfer" || is_call_op(b, kJettonTransfer))) {
      if (!leads_to_hop(b)) p.out_transfers.push_back(b);  // intermediates unused downstream
    }
  }
  std::stable_sort(p.out_transfers.begin(), p.out_transfers.end(), by_lt);
  return p;
}

}  // namespace

// builders/tonco_swap.py _tonco_swap_data -> blocks/swaps.py build_tonco_swap_core
// (§11.5 full port). args = (consumed list). Deterministic: the jetton legs are
// read by btype/`.data` (swaps.py:856/926), the pTON in-leg from a single-leaf
// call message. Multi-hop peer_swaps + the failed-swap recursive-payload
// fallback are ported faithfully (no corpus coverage — single-hop only).
EvalResult tonco_swap_data(BuildEnv &env, const std::vector<Value> &args) {
  if (args.size() != 1 || args[0].t != VType::List) {
    return rt_ok(Value::null());
  }
  std::vector<const Block *> consumed;
  for (const Value &v : *args[0].items) {
    const Block *b = as_block(v);
    if (b != nullptr) consumed.push_back(b);
  }
  if (consumed.empty()) return rt_ok(Value::null());
  ToncoParts parts = derive_tonco_parts(consumed);

  auto by_lt = [](const Block *a, const Block *b) { return a->min_lt < b->min_lt; };
  std::vector<const Block *> all_swaps = parts.peer_swaps;
  all_swaps.insert(all_swaps.begin(), parts.anchor);
  std::stable_sort(all_swaps.begin(), all_swaps.end(), by_lt);
  const std::vector<const Block *> &all_payouts = parts.payouts;
  if (parts.in_transfer == nullptr || all_swaps.size() != all_payouts.size()) {
    return rt_ok(Value::null());
  }
  const Block *in_transfer_block = parts.in_transfer;

  bool ok = true;
  struct Step {
    Value source_wallet;
    RouterPayTo pay;
    std::int64_t min_lt;
  };
  std::vector<Step> swap_steps;
  std::map<std::string, Value> jw_asset_map;
  for (std::size_t i = 0; i < all_swaps.size(); i++) {
    auto r_sbody = block_body(all_swaps[i]);
    if (r_sbody.is_error()) { ok = false; continue; }
    auto r_sw = parse_v3_swap_source_wallet(r_sbody.move_as_ok());
    if (r_sw.is_error()) { ok = false; continue; }
    auto r_pbody = block_body(all_payouts[i]);
    if (r_pbody.is_error()) { ok = false; continue; }
    auto r_pt = parse_v3_pay_to(r_pbody.move_as_ok());
    if (r_pt.is_error()) { ok = false; continue; }
    RouterPayTo pt = r_pt.move_as_ok();
    if (pt.exit_code != 0 && pt.exit_code != 200) ok = false;
    Value source_wallet = r_sw.move_as_ok();
    std::string key = acc_str_t(source_wallet).value_or("");
    swap_steps.push_back({source_wallet, pt, all_swaps[i]->min_lt});
    if (jw_asset_map.find(key) == jw_asset_map.end()) {
      Value asset = wallet_asset(env, key);  // pton -> TON, jetton, or Null on miss
      if (!asset.is_null()) jw_asset_map[key] = asset;
    }
  }
  std::stable_sort(swap_steps.begin(), swap_steps.end(),
                   [](const Step &a, const Step &b) { return a.min_lt < b.min_lt; });

  // --- incoming transfer ---
  Value sender, in_asset, in_amount, in_source, in_source_jw, in_dest, in_dest_jw;
  td::Ref<vm::Cell> in_payload;
  if (in_transfer_block->btype == "jetton_transfer") {
    in_asset = tonco_leg_asset(data_field(in_transfer_block, "asset"));
    sender = data_field(in_transfer_block, "sender");
    in_amount = data_field(in_transfer_block, "amount");
    in_source = data_field(in_transfer_block, "sender");
    in_source_jw = data_field(in_transfer_block, "sender_wallet");
    in_dest = data_field(in_transfer_block, "receiver");
    in_dest_jw = data_field(in_transfer_block, "receiver_wallet");
    Value fp = data_field(in_transfer_block, "forward_payload");
    if (fp.t == VType::Cell && fp.cell.not_null()) in_payload = fp.cell;
  } else {
    const Message *m = block_msg(in_transfer_block);
    if (m == nullptr) return rt_ok(Value::null());  // except -> None
    td::RefInt256 amt = m->value ? td::make_refint(*m->value) : td::RefInt256();
    if (m->opcode32() && *m->opcode32() == kPTonTransfer) {
      auto rb = block_body(in_transfer_block);
      if (rb.is_error()) return rt_ok(Value::null());
      auto rp = parse_message_body("PTonTransfer", rb.ok());
      if (rp.is_error()) return rt_ok(Value::null());
      const Value *ta = rp.ok().field("ton_amount");
      amt = (ta != nullptr && (ta->t == VType::Int || ta->t == VType::Amount)) ? ta->num
                                                                              : td::RefInt256();
      const Value *fpv = rp.ok().field("forward_payload");
      if (fpv != nullptr && fpv->t == VType::Cell && fpv->cell.not_null()) in_payload = fpv->cell;
    }
    sender = account_from_opt(m->source);
    in_asset = Value::make_asset_ton();
    in_amount = amount_or_zero_t(amt);
    in_source = account_from_opt(m->source);
    in_source_jw = Value::null();
    const Message *am = block_msg(parts.anchor);
    in_dest = account_from_opt(am != nullptr ? am->source : std::nullopt);
    in_dest_jw = account_from_opt(m->destination);
  }

  // --- outgoing transfer ---
  const Block *out_transfer = parts.out_transfers.empty() ? nullptr : parts.out_transfers.back();
  if (out_transfer == nullptr) return rt_ok(Value::null());
  if (out_transfer->btype != "jetton_transfer") {
    return rt_ok(Value::null());  // unsupported out type
  }
  Value out_asset = tonco_leg_asset(data_field(out_transfer, "asset"));
  Value out_receiver_jw = data_field(out_transfer, "receiver_wallet");
  Value out_sender_jw = data_field(out_transfer, "sender_wallet");
  if (auto rw = acc_str_t(out_receiver_jw)) jw_asset_map[*rw] = out_asset;
  if (auto sw = acc_str_t(out_sender_jw)) jw_asset_map[*sw] = out_asset;
  Value out_amount = data_field(out_transfer, "amount");
  Value out_source = data_field(out_transfer, "sender");
  Value out_dest = data_field(out_transfer, "receiver");

  // --- peer_swaps (multi-hop only; empty for single-hop corpus) ---
  auto payout_assets = [](const RouterPayTo &pt) {
    std::vector<std::pair<td::RefInt256, Value>> a;
    if (!pt.amount0.is_null() && pt.jetton0.t == VType::Account) a.emplace_back(pt.amount0, pt.jetton0);
    if (!pt.amount1.is_null() && pt.jetton1.t == VType::Account) a.emplace_back(pt.amount1, pt.jetton1);
    std::stable_sort(a.begin(), a.end(), [](const auto &x, const auto &y) {
      td::RefInt256 xa = x.first.is_null() ? td::make_refint(0) : x.first;
      td::RefInt256 ya = y.first.is_null() ? td::make_refint(0) : y.first;
      return td::cmp(xa, ya) > 0;  // reverse
    });
    return a;
  };
  auto leg = [](const Value &amount, const Value &asset) {
    return Value::make_dict(Value::Fields{{"amount", amount}, {"asset", asset}});
  };
  std::vector<Value> peer_swap_data;
  if (swap_steps.size() > 1) {
    auto a0 = payout_assets(swap_steps[0].pay);
    if (!a0.empty()) {
      auto out_addr = acc_str_t(a0[0].second).value_or("");
      auto it = jw_asset_map.find(out_addr);
      if (it != jw_asset_map.end()) {
        Value prev_out = leg(amount_or_zero_t(a0[0].first), it->second);
        peer_swap_data.push_back(Value::make_dict(
            Value::Fields{{"in", leg(in_amount, in_asset)}, {"out", prev_out}}));
        for (std::size_t i = 1; i < swap_steps.size(); i++) {
          const RouterPayTo &np = swap_steps[i].pay;
          if (np.exit_code != 0 && np.exit_code != 200) continue;
          auto an = payout_assets(np);
          if (an.empty()) continue;
          auto na = acc_str_t(an[0].second).value_or("");
          auto nit = jw_asset_map.find(na);
          Value next_asset = nit != jw_asset_map.end() ? nit->second : Value::make_asset_ton();
          Value next_out = leg(amount_or_zero_t(an[0].first), next_asset);
          peer_swap_data.push_back(
              Value::make_dict(Value::Fields{{"in", prev_out}, {"out", next_out}}));
          prev_out = next_out;
        }
      }
    }
  }

  // --- destination asset / min_out (+ failed-swap recursive-payload fallback) ---
  Value destination_asset = out_asset;
  Value min_out_amount = out_amount;
  if (!ok && !swap_steps.empty() && in_payload.not_null()) {
    std::vector<std::pair<std::string, td::RefInt256>> targets;
    try {
      vm::CellSlice cs = vm::load_cell_slice(in_payload);
      collect_payload_targets(cs, targets, 0);
    } catch (...) {
    }
    if (!targets.empty()) {
      const std::string &twallet = targets.back().first;
      Value target_asset;
      auto it = jw_asset_map.find(twallet);
      if (it != jw_asset_map.end()) target_asset = it->second;
      if (target_asset.is_null()) {
        Value ta = wallet_asset(env, twallet);
        if (!ta.is_null()) target_asset = ta;
      }
      if (!target_asset.is_null()) {
        destination_asset = target_asset;
        min_out_amount = amount_or_zero_t(targets.back().second);
      }
    }
  }

  Value::Fields incoming{{"asset", in_asset},
                         {"amount", in_amount},
                         {"source", in_source},
                         {"source_jetton_wallet", in_source_jw},
                         {"destination", in_dest},
                         {"destination_jetton_wallet", in_dest_jw}};
  Value::Fields outgoing{{"asset", out_asset},
                         {"amount", out_amount},
                         {"source", out_source},
                         {"source_jetton_wallet", out_sender_jw},
                         {"destination", out_dest},
                         {"destination_jetton_wallet", out_receiver_jw}};

  Value::Fields rec;
  rec.emplace_back("dex", Value::make_str("tonco"));
  rec.emplace_back("source_asset", in_asset);
  rec.emplace_back("destination_asset", std::move(destination_asset));
  rec.emplace_back("sender", sender);
  rec.emplace_back("dex_incoming_transfer", Value::make_dict(std::move(incoming)));
  rec.emplace_back("dex_outgoing_transfer", Value::make_dict(std::move(outgoing)));
  rec.emplace_back("referral_amount", Value::null());
  rec.emplace_back("referral_address", Value::null());
  rec.emplace_back("peer_swaps", Value::make_list(std::move(peer_swap_data)));
  rec.emplace_back("min_out_amount", std::move(min_out_amount));
  rec.emplace_back("failed", Value::make_bool(!ok));
  return rt_ok(Value::make_obj(std::move(rec)));
}

}  // namespace mch
