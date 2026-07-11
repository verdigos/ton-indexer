// DeDust liquidity-deposit host fns (builders/dedust.py _dedust_deposit_* +
// blocks/liquidity.py build_dedust_deposit_{liquidity,partial}_core). See
// host/HostImpls.h for the internal registry surface.
//
// Both fns take the consumed set and reconstruct the legacy label roles by
// previous_block / children_blocks navigation. They return the field record
// (Obj) on success or Null to reject (spec §10.7) — reproducing the Python
// `return None` (its try/except swallows every exception into None, so any
// parse/nav failure here is likewise a Null reject).
#include "host/HostImpls.h"

#include "host/HostCommon.h"

#include "BlockTree.h"
#include "BuildRuntime.h"
#include "MsgParse.h"
#include "parse/PSlice.h"

#include "vm/cellslice.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace mch {

namespace {

constexpr std::uint32_t kJettonTransfer = 0x0f8a7ea5;
constexpr std::uint32_t kJettonInternalTransfer = 0x178d4519;
constexpr std::uint32_t kJettonNotify = 0x7362d09c;
constexpr std::uint32_t kDepositTONToVault = 0xd55e4686;
constexpr std::uint32_t kTopUp = 0x54240fe5;
constexpr std::uint32_t kDepositLiquidityToPool = 0xb56b9598;
constexpr std::uint32_t kReturnExcessFromVault = 0x6b0b787f;
constexpr std::uint32_t kDedustPayout = 0x474f86cf;
constexpr std::uint32_t kRejection = 0xe1a36cd4;
constexpr std::uint32_t kJettonForwardPayload = 0x40e108d6;

std::string hex_upper_b(const unsigned char *p, std::size_t n) {
  static const char *d = "0123456789ABCDEF";
  std::string out(n * 2, '0');
  for (std::size_t i = 0; i < n; i++) {
    out[2 * i] = d[p[i] >> 4];
    out[2 * i + 1] = d[p[i] & 0xF];
  }
  return out;
}

// messages/liquidity.py load_asset: 4-bit kind (0 -> TON, else wc:uint8 +
// account_id:32 bytes). Advances cs.
td::Result<Value> load_asset_4(vm::CellSlice &cs) {
  if (!cs.have(4)) {
    return td::Status::Error("asset: kind underflow");
  }
  auto kind = cs.fetch_ulong(4);
  if (kind == 0) {
    return Value::make_asset_ton();
  }
  if (!cs.have(8 + 256)) {
    return td::Status::Error("asset: jetton underflow");
  }
  auto wc = cs.fetch_ulong(8);
  unsigned char id[32];
  if (!cs.fetch_bytes(id, 32)) {
    return td::Status::Error("asset: id fetch");
  }
  return Value::make_asset_jetton(std::to_string(wc) + ":" + hex_upper_b(id, 32));
}

template <typename Vec>
const Block *find_call(const Vec &blocks, std::uint32_t op) {
  for (const Block *b : blocks) {
    if (is_call_op(b, op)) {
      return b;
    }
  }
  return nullptr;
}

// Amount(x or 0): coins are never None, and `0 or 0 == 0`, so this is just
// Amount(x) — but keep the null-guard for parse-failure paths that reach here.
Value amount_or_zero(td::RefInt256 v) {
  return v.is_null() ? Value::make_amount(td::make_refint(0)) : Value::make_amount(std::move(v));
}

// _dedust_deposit_leg resolution result.
struct Leg {
  const Block *deposit{nullptr};       // vault-facing call the cores parse
  std::vector<const Block *> jt_calls; // raw user JettonTransfer calls
  bool jt_none{false};                 // jt_calls must come from consumed
};

// builders/dedust.py _dedust_deposit_leg.
Leg resolve_leg(const Block *leg) {
  Leg out;
  if (is_call_op(leg, kDepositTONToVault)) {
    out.deposit = leg;
    return out;  // jt_calls = []
  }
  if (leg != nullptr && leg->btype == "jetton_transfer") {
    out.deposit = find_call(leg->children_blocks, kJettonNotify);
    for (const Block *b : leg->children_blocks) {
      if (is_call_op(b, kJettonTransfer)) {
        out.jt_calls.push_back(b);
      }
    }
    return out;
  }
  out.deposit = leg;
  out.jt_none = true;
  return out;
}

// DedustDepositLiquidityToPool(body): owner_addr, asset0, asset0_amount,
// asset1, asset1_amount. proof + field4 are refs (in order).
struct PoolDeposit {
  Value owner;
  Value asset0, asset1;
  td::RefInt256 asset0_amount, asset1_amount;
};
td::Result<PoolDeposit> parse_pool_deposit(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64) || !cs.advance(32 + 64)) {
    return td::Status::Error("pool deposit: header underflow");
  }
  if (cs.size_refs() < 1) {
    return td::Status::Error("pool deposit: proof ref missing");
  }
  cs.fetch_ref();  // proof (discarded)
  PoolDeposit pd;
  TRY_RESULT_ASSIGN(pd.owner, load_address_py(cs));
  TRY_RESULT(min_lp, load_coins_py(cs));
  (void)min_lp;
  if (cs.size_refs() < 1) {
    return td::Status::Error("pool deposit: field4 ref missing");
  }
  bool special = false;
  vm::CellSlice f4;
  try {
    f4 = vm::load_cell_slice_special(cs.fetch_ref(), special);
  } catch (...) {
    return td::Status::Error("pool deposit: field4 slice");
  }
  TRY_RESULT_ASSIGN(pd.asset0, load_asset_4(f4));
  TRY_RESULT_ASSIGN(pd.asset0_amount, load_coins_py(f4));
  TRY_RESULT_ASSIGN(pd.asset1, load_asset_4(f4));
  TRY_RESULT_ASSIGN(pd.asset1_amount, load_coins_py(f4));
  return pd;
}

}  // namespace

// blocks/liquidity.py build_dedust_deposit_liquidity_core, wired to
// _dedust_deposit_final_data's structural role derivation.
EvalResult dedust_deposit_final_data(BuildEnv &env, const std::vector<Value> &args) {
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

  // anchor <- topup <- ask <- deposit leg (None on any missing link ==
  // Python AttributeError on None.previous_block -> return None).
  const Block *p1 = block->previous_block;
  const Block *p2 = p1 != nullptr ? p1->previous_block : nullptr;
  const Block *leg = p2 != nullptr ? p2->previous_block : nullptr;
  if (leg == nullptr) {
    return rt_ok(Value::null());
  }
  Leg lg = resolve_leg(leg);
  std::vector<const Block *> jt_calls = lg.jt_calls;
  if (lg.jt_none) {
    jt_calls.clear();
    for (const Block *b : consumed) {
      if (is_call_op(b, kJettonTransfer)) {
        jt_calls.push_back(b);
      }
    }
  }

  // lp_transfer / rejection: consumed[1:] whose previous_block is the anchor.
  const Block *lp_transfer = nullptr;
  const Block *rejection = nullptr;
  for (std::size_t i = 1; i < consumed.size(); i++) {
    const Block *b = consumed[i];
    if (lp_transfer == nullptr && is_call_op(b, kJettonInternalTransfer) &&
        b->previous_block == block) {
      lp_transfer = b;
    }
    if (rejection == nullptr && is_call_op(b, kRejection) && b->previous_block == block) {
      rejection = b;
    }
  }
  // returns / ton_excesses. jetton_excesses is the isinstance(JettonTransferBlock)
  // scan — a LEGACY class that never appears under the IR engine, so the list is
  // always empty (dead branch, reproduced as []).
  std::vector<const Block *> returns;
  for (const Block *b : consumed) {
    if (is_call_op(b, kReturnExcessFromVault)) {
      returns.push_back(b);
    }
  }
  auto in_returns = [&](const Block *p) {
    return std::find(returns.begin(), returns.end(), p) != returns.end();
  };
  std::vector<const Block *> ton_excesses;
  for (const Block *b : consumed) {
    if (is_call_op(b, kDedustPayout) && in_returns(b->previous_block)) {
      ton_excesses.push_back(b);
    }
  }

  // --- build_dedust_deposit_liquidity_core ---
  auto r_anchor_body = block_body(block);
  if (r_anchor_body.is_error()) {
    return rt_ok(Value::null());
  }
  auto r_pd = parse_pool_deposit(r_anchor_body.move_as_ok());
  if (r_pd.is_error()) {
    return rt_ok(Value::null());
  }
  PoolDeposit pd = r_pd.move_as_ok();
  Value sender = pd.owner;  // AccountId(owner_addr)
  const Message *anchor_msg = block_msg(block);
  Value deposit_contract = account_from_opt(anchor_msg != nullptr ? anchor_msg->source
                                                                  : std::nullopt);

  Value lpool;
  Value lp_tokens;
  if (lp_transfer != nullptr) {
    auto r_body = block_body(lp_transfer);
    if (r_body.is_error()) {
      return rt_ok(Value::null());
    }
    auto r = parse_message_body("JettonInternalTransfer", r_body.ok());
    if (r.is_error()) {
      return rt_ok(Value::null());
    }
    const Value *amt = r.ok().field("amount");
    lpool = account_from_opt(anchor_msg != nullptr ? anchor_msg->destination : std::nullopt);
    lp_tokens = amt != nullptr ? to_amount(*amt) : Value::make_amount_none();
  } else if (rejection != nullptr) {
    const Message *rm = block_msg(rejection);
    lpool = account_from_opt(rm != nullptr ? rm->source : std::nullopt);
    lp_tokens = Value::null();  // None
  } else {
    return rt_ok(Value::null());  // raise -> return None
  }

  // actual asset/amount from the deposit leg head.
  Value actual_asset = Value::make_asset_ton();
  Value actual_amount = Value::make_amount_none();
  if (is_call_op(lg.deposit, kDepositTONToVault)) {
    auto r_body = block_body(lg.deposit);
    if (r_body.is_error()) {
      return rt_ok(Value::null());
    }
    auto r_ctx = open_body(r_body.ok());
    if (r_ctx.is_error()) {
      return rt_ok(Value::null());
    }
    auto ctx = r_ctx.move_as_ok();
    auto &cs = ctx.cs;
    if (!cs.have(32 + 64) || !cs.advance(32 + 64)) {
      return rt_ok(Value::null());
    }
    auto r_amt = load_coins_py(cs);
    if (r_amt.is_error()) {
      return rt_ok(Value::null());
    }
    actual_amount = Value::make_amount(r_amt.move_as_ok());
  } else {
    if (lg.deposit == nullptr) {
      return rt_ok(Value::null());  // deposit.get_body() on None -> except
    }
    auto r_body = block_body(lg.deposit);
    if (r_body.is_error()) {
      return rt_ok(Value::null());
    }
    auto r = parse_message_body("JettonNotify", r_body.ok());
    if (r.is_error()) {
      return rt_ok(Value::null());
    }
    const Value *ja = r.ok().field("jetton_amount");
    actual_amount = ja != nullptr ? to_amount(*ja) : Value::make_amount_none();
    const Message *dm = block_msg(lg.deposit);
    if (dm == nullptr || !dm->source) {
      return rt_ok(Value::null());
    }
    Value jw = env.lookups->get("jetton_wallet", std::vector<Value>{Value::make_str(*dm->source)});
    const Value *jetton = jw.is_null() ? nullptr : jw.field("jetton");
    if (jetton == nullptr || jetton->t != VType::Str) {
      return rt_ok(Value::null());  // wallet_info.jetton AttributeError -> None
    }
    auto norm = normalize_raw_address(jetton->str);
    actual_asset = Value::make_asset_jetton(norm ? *norm : jetton->str);
  }

  // user_jetton_wallet_0: the jetton deposit whose wallet's jetton == actual_asset.
  Value user_jw_0 = Value::make_account_none();
  Value user_jw_1 = Value::make_account_none();  // never resolvable
  std::string actual_master = actual_asset.t == VType::Asset && !actual_asset.is_ton
                                  ? actual_asset.str
                                  : std::string{};
  for (const Block *jdep : jt_calls) {
    const Message *jm = block_msg(jdep);
    if (jm == nullptr || !jm->source) {
      continue;
    }
    // filter: message.source.upper() == sender.as_str()
    Value jsrc = account_from_opt(jm->source);
    if (!(sender.t == VType::Account && jsrc.t == VType::Account && !sender.addr_none &&
          !jsrc.addr_none && jsrc.str == sender.str)) {
      continue;
    }
    if (!jm->destination) {
      continue;
    }
    Value jw = env.lookups->get("jetton_wallet",
                                std::vector<Value>{Value::make_str(*jm->destination)});
    if (jw.is_null()) {
      continue;
    }
    const Value *jetton = jw.field("jetton");
    if (jetton == nullptr || jetton->t != VType::Str) {
      continue;
    }
    auto norm = normalize_raw_address(jetton->str);
    std::string jw_master = norm ? *norm : jetton->str;
    if (!actual_master.empty() && jw_master == actual_master) {
      user_jw_0 = account_from_opt(*jm->destination);
    }
  }

  // vault_excesses: TON excesses to the sender (jetton_excesses dead -> []).
  std::vector<Value> excesses;
  for (const Block *te : ton_excesses) {
    const Message *tm = block_msg(te);
    if (tm == nullptr) {
      continue;
    }
    Value dst = account_from_opt(tm->destination);
    if (dst.t == VType::Account && !dst.addr_none && sender.t == VType::Account &&
        !sender.addr_none && dst.str == sender.str) {
      Value amt = tm->value ? Value::make_amount(td::make_refint(*tm->value))
                            : Value::make_amount_none();
      excesses.push_back(Value::make_list(
          std::vector<Value>{Value::make_asset_ton(), std::move(amt)}));
    }
  }

  bool success = !lp_tokens.is_null();
  Value::Fields d;
  d.emplace_back("sender", sender);
  d.emplace_back("pool_address", std::move(lpool));
  d.emplace_back("deposit_contract", std::move(deposit_contract));
  d.emplace_back("lp_tokens_minted", lp_tokens);
  d.emplace_back("asset_1", actual_asset);
  d.emplace_back("asset_2", Value::null());
  d.emplace_back("amount_1", std::move(actual_amount));
  d.emplace_back("amount_2", Value::null());
  d.emplace_back("target_asset_1", pd.asset0);
  d.emplace_back("target_amount_1", amount_or_zero(pd.asset0_amount));
  d.emplace_back("target_asset_2", pd.asset1);
  d.emplace_back("target_amount_2", amount_or_zero(pd.asset1_amount));
  d.emplace_back("user_jetton_wallet_1", std::move(user_jw_0));
  d.emplace_back("user_jetton_wallet_2", std::move(user_jw_1));
  d.emplace_back("vault_excesses", Value::make_list(std::move(excesses)));
  d.emplace_back("success", Value::make_bool(success));
  return rt_ok(Value::make_obj(std::move(d)));
}

// blocks/liquidity.py build_dedust_deposit_partial_core.
EvalResult dedust_deposit_partial_data(BuildEnv &env, const std::vector<Value> &args) {
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
  const Block *block = consumed.front();  // topup anchor

  const Block *p1 = block->previous_block;
  const Block *leg = p1 != nullptr ? p1->previous_block : nullptr;
  if (leg == nullptr) {
    return rt_ok(Value::null());
  }
  Leg lg = resolve_leg(leg);
  const Block *vault_call = lg.deposit;

  // Guard: final-deposit opcodes after the topup -> raise -> reject.
  const Block *topup = find_call(consumed, kTopUp);
  if (topup == nullptr) {
    return rt_ok(Value::null());  // find_call_contract None -> .next_blocks AttributeError
  }
  for (const Block *n : topup->next_blocks) {
    if (is_call_op(n, kDepositLiquidityToPool) || is_call_op(n, kRejection)) {
      return rt_ok(Value::null());  // Unexpected call contract after deposit top up
    }
  }

  const Message *bm = block_msg(block);
  Value deposit_contract = account_from_opt(bm != nullptr ? bm->destination : std::nullopt);

  Value sender;
  Value actual_asset = Value::make_asset_ton();
  Value actual_amount = Value::make_amount_none();
  Value asset0, asset1;
  td::RefInt256 asset0_amount, asset1_amount;
  Value user_jw_0 = Value::make_account_none();
  Value user_jw_1 = Value::make_account_none();

  if (vault_call == nullptr) {
    return rt_ok(Value::null());  // vault_call.get_message() on None -> except -> None
  }
  auto r_body = block_body(vault_call);
  if (r_body.is_error()) {
    return rt_ok(Value::null());
  }

  // Python tries DedustDepositTONToVault first; on ANY failure falls to the
  // jetton branch. Distinguish by opcode (a TON-to-vault call parses cleanly,
  // else it is a JettonNotify).
  bool ton_branch = is_call_op(vault_call, kDepositTONToVault);
  if (ton_branch) {
    const Message *vm_msg = block_msg(vault_call);
    sender = account_from_opt(vm_msg != nullptr ? vm_msg->source : std::nullopt);
    // amount, pool_type bit, asset0, asset1, ref{min_lp, a0_target, a1_target}.
    auto r_ctx2 = open_body(r_body.ok());
    if (r_ctx2.is_error()) {
      return rt_ok(Value::null());
    }
    auto ctx2 = r_ctx2.move_as_ok();
    auto &c2 = ctx2.cs;
    if (!c2.have(32 + 64) || !c2.advance(32 + 64)) {
      return rt_ok(Value::null());
    }
    auto r_amt = load_coins_py(c2);
    if (r_amt.is_error() || !c2.have(1)) {
      return rt_ok(Value::null());
    }
    actual_amount = Value::make_amount(r_amt.move_as_ok());
    c2.advance(1);  // pool_type bit
    auto r_a0 = load_asset_4(c2);
    auto r_a1 = load_asset_4(c2);
    if (r_a0.is_error() || r_a1.is_error() || c2.size_refs() < 1) {
      return rt_ok(Value::null());
    }
    asset0 = r_a0.move_as_ok();
    asset1 = r_a1.move_as_ok();
    bool special = false;
    vm::CellSlice pr;
    try {
      pr = vm::load_cell_slice_special(c2.fetch_ref(), special);
    } catch (...) {
      return rt_ok(Value::null());
    }
    auto r_min = load_coins_py(pr);
    auto r_t0 = load_coins_py(pr);
    auto r_t1 = load_coins_py(pr);
    if (r_min.is_error() || r_t0.is_error() || r_t1.is_error()) {
      return rt_ok(Value::null());
    }
    asset0_amount = r_t0.move_as_ok();
    asset1_amount = r_t1.move_as_ok();
  } else {
    // jetton deposit branch: JettonNotify -> forward payload.
    auto r = parse_message_body("JettonNotify", r_body.ok());
    if (r.is_error()) {
      return rt_ok(Value::null());
    }
    Value notif = r.move_as_ok();
    const Value *ja = notif.field("jetton_amount");
    const Value *fu = notif.field("from_user");
    const Value *fp = notif.field("forward_payload_cell");
    sender = fu != nullptr ? *fu : Value::make_account_none();
    actual_amount = ja != nullptr ? to_amount(*ja) : Value::make_amount_none();
    if (fp == nullptr || fp->t != VType::Cell) {
      return rt_ok(Value::null());  // load_ref().begin_parse() on None -> except
    }
    bool special = false;
    vm::CellSlice cs;
    try {
      cs = vm::load_cell_slice_special(fp->cell, special);
    } catch (...) {
      return rt_ok(Value::null());
    }
    if (!cs.have(32) || cs.fetch_ulong(32) != kJettonForwardPayload || !cs.have(1)) {
      return rt_ok(Value::null());
    }
    cs.advance(1);  // pool_type bit
    auto r_a0 = load_asset_4(cs);
    auto r_a1 = load_asset_4(cs);
    if (r_a0.is_error() || r_a1.is_error()) {
      return rt_ok(Value::null());
    }
    asset0 = r_a0.move_as_ok();
    asset1 = r_a1.move_as_ok();
    auto r_min = load_coins_py(cs);
    auto r_t0 = load_coins_py(cs);
    auto r_t1 = load_coins_py(cs);
    if (r_min.is_error() || r_t0.is_error() || r_t1.is_error()) {
      return rt_ok(Value::null());
    }
    asset0_amount = r_t0.move_as_ok();
    asset1_amount = r_t1.move_as_ok();

    // user_asset_wallet_0 = vault_call.previous_block.get_message().source
    const Block *vprev = vault_call->previous_block;
    if (vprev == nullptr) {
      return rt_ok(Value::null());
    }
    const Message *vpm = block_msg(vprev);
    user_jw_0 = account_from_opt(vpm != nullptr ? vpm->source : std::nullopt);

    // actual_asset from the dex jetton wallet (vault_call source).
    const Message *vm_msg = block_msg(vault_call);
    if (vm_msg == nullptr || !vm_msg->source) {
      return rt_ok(Value::null());
    }
    Value jw = env.lookups->get("jetton_wallet",
                                std::vector<Value>{Value::make_str(*vm_msg->source)});
    const Value *jetton = jw.is_null() ? nullptr : jw.field("jetton");
    if (jetton == nullptr || jetton->t != VType::Str) {
      return rt_ok(Value::null());  // dex_jetton_wallet.jetton AttributeError -> None
    }
    auto norm = normalize_raw_address(jetton->str);
    actual_asset = Value::make_asset_jetton(norm ? *norm : jetton->str);
  }

  Value::Fields d;
  d.emplace_back("sender", (sender.t == VType::Account) ? sender : Value::make_account_none());
  d.emplace_back("deposit_contract", std::move(deposit_contract));
  d.emplace_back("asset_1", std::move(actual_asset));
  d.emplace_back("amount_1", std::move(actual_amount));
  d.emplace_back("asset_2", Value::null());
  d.emplace_back("amount_2", Value::null());
  d.emplace_back("user_jetton_wallet_1", std::move(user_jw_0));
  d.emplace_back("user_jetton_wallet_2", std::move(user_jw_1));
  d.emplace_back("target_asset_1", std::move(asset0));
  d.emplace_back("target_amount_1", amount_or_zero(asset0_amount));
  d.emplace_back("target_asset_2", std::move(asset1));
  d.emplace_back("target_amount_2", amount_or_zero(asset1_amount));
  return rt_ok(Value::make_obj(std::move(d)));
}

}  // namespace mch
