// Tonco liquidity-deposit + swap host fns (builders/tonco.py, builders/tonco_swap.py
// + blocks/liquidity.py build_tonco_deposit_liquidity_core / blocks/swaps.py
// build_tonco_swap_core). See host/HostImpls.h for the internal registry surface.
#include "host/HostImpls.h"

#include "host/HostCommon.h"

#include "BlockTree.h"
#include "BuildRuntime.h"
#include "MsgParse.h"
#include "parse/PSlice.h"

#include "common/refint.h"
#include "vm/cellslice.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mch {

namespace {

constexpr std::uint32_t kJettonNotify = 0x7362d09c;
constexpr std::uint32_t kFundAccountPayload = 0x4468de77;

const std::vector<std::string> &tonco_pton_masters() {
  static const std::vector<std::string> m = {
      "0:8CDC1D7640AD5EE326527FC1AD0514F468B30DC84B0173F0E155F451B4E11F7C",
      "0:671963027F7F85659AB55B821671688601CDCF1EE674FC7FBBB1A776A18D34A3",
      "0:949C4C66760C002800E2FA3D8A3CA4E1C90A9373B53AE7472033483BF14CD95E"};
  return m;
}
bool tonco_is_pton(const std::string &s) {
  const auto &m = tonco_pton_masters();
  for (const auto &x : m) {
    if (x == s) {
      return true;
    }
  }
  return false;
}

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
  if (jw.is_null()) {
    return Value::null();
  }
  const Value *jf = jw.field("jetton");
  if (jf == nullptr || jf->t != VType::Str) {
    return Value::null();
  }
  if (tonco_is_pton(jf->str)) {
    return Value::make_asset_ton();
  }
  auto norm = normalize_raw_address(jf->str);
  return Value::make_asset_jetton(norm ? *norm : jf->str);
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
  // excess_transfers isinstance(JettonTransferBlock) is DEAD under the IR engine
  // (generic Block) -> the excesses loop `else: continue`s every element, so the
  // excesses list is always empty. (No lookup, no ordering to reproduce.)

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
  d.emplace_back("excesses", Value::make_list({}));
  return rt_ok(Value::make_obj(std::move(d)));
}

// builders/tonco_swap.py _tonco_swap_data -> blocks/swaps.py build_tonco_swap_core.
// Under the IR engine this fn ALWAYS rejects: the core resolves the outgoing
// transfer with `isinstance(out_transfer, JettonTransferBlock)` (swaps.py:926) —
// a LEGACY class. out_transfer is always a generic jetton_transfer Block or a
// raw JettonTransfer call under the IR engine, so the isinstance is False and
// the else arm `return None` (swaps.py:962-966) fires; the core's only other
// exits are also None (empty out_transfers, in-leg parse failure). _tonco_swap_data
// then returns None -> reject. (The in-leg is processed first and reads
// in_transfer.event_nodes[0].message non-deterministically, but that value is
// discarded on the reject, so the rejection is deterministic.) The tonco-swap
// pattern also never matches any corpus trace, so this fn is never even invoked
// there. Wired for surface parity; the SKIP->included flip adds zero ACTIONs.
EvalResult tonco_swap_data(BuildEnv &, const std::vector<Value> &args) {
  if (args.size() != 1) {
    return rt_ok(Value::null());
  }
  return rt_ok(Value::null());  // out_transfer isinstance JettonTransferBlock dead -> None
}

}  // namespace mch
