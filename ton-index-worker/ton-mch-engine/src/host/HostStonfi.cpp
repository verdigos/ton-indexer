// Stonfi v1/v2 host bindings (builders/stonfi_v1.py, builders/stonfi_v2.py +
// blocks/swaps.py cores). See host/HostImpls.h for the internal registry
// surface and HostRegistry.h for the public one.
#include "host/HostImpls.h"

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
#include <string>
#include <utility>
#include <vector>

namespace mch {

namespace {

constexpr std::uint32_t kStonfiPaymentRequest = 0xf93bb43f;
constexpr std::uint32_t kStonfiSwapOk = 0xc64370e5;
constexpr std::uint32_t kStonfiSwapNoLiq = 0x5ffe1295;
constexpr std::uint32_t kStonfiSwapReserveErr = 0x38976e9b;

// stonfi v2 opcodes (blocks/swaps.py StonfiV2SwapBlockMatcher). The swap
// REQUEST call carries op 0x6664de2a (swap_opcode) with a StonfiSwapV2 body;
// the pay_to call carries op 0x657b54f5 with a StonfiV2PayTo body (the
// StonfiSwapV2.opcode class attr is misleading/unused — its __init__ never
// checks the tag). 0x6664de2a doubles as the forward-payload swap opcode and
// one of the cross-swap sum-type tags.
constexpr std::uint32_t kV2Swap = 0x6664de2a;       // swap request call
constexpr std::uint32_t kV2PayTo = 0x657b54f5;      // pay_to call
constexpr std::uint32_t kV2CrossSwapB = 0x69cf1a5b;
constexpr std::uint32_t kPTonTransfer = 0x01f3835d;

const std::vector<std::string> &pton_masters() {
  static const std::vector<std::string> m = {
      "0:8CDC1D7640AD5EE326527FC1AD0514F468B30DC84B0173F0E155F451B4E11F7C",
      "0:671963027F7F85659AB55B821671688601CDCF1EE674FC7FBBB1A776A18D34A3",
      "0:949C4C66760C002800E2FA3D8A3CA4E1C90A9373B53AE7472033483BF14CD95E"};
  return m;
}
bool is_pton_master(const std::string &s) {
  const auto &m = pton_masters();
  return std::find(m.begin(), m.end(), s) != m.end();
}

// account -> canonical "wc:HEX" string, or nullopt for addr_none/non-account.
std::optional<std::string> acc_str(const Value &v) {
  if (v.t == VType::Account && !v.addr_none) {
    return v.str;
  }
  return std::nullopt;
}

// messages/swaps.py StonfiPaymentRequest.__init__ IN FULL: opcode(32),
// query_id(64), owner(load_address), exit_code(32), then a ref whose slice
// carries amount0/token0/amount1/token1. Python constructs the whole object
// before the caller reads .exit_code, so any failure past exit_code (e.g. a
// missing ref) still raises -> the pred's try/except returns False. Reproduce
// the full parse and only report exit_code when every field parsed.
td::Result<std::uint32_t> stonfi_payment_exit_code(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64) || !cs.advance(32 + 64)) {
    return td::Status::Error("stonfi payment: header underflow");
  }
  TRY_RESULT(owner, load_address_py(cs));
  (void)owner;
  if (!cs.have(32)) {
    return td::Status::Error("stonfi payment: exit_code underflow");
  }
  auto exit_code = static_cast<std::uint32_t>(cs.fetch_ulong(32));
  if (cs.size_refs() < 1) {
    return td::Status::Error("stonfi payment: ref missing");
  }
  bool special = false;
  vm::CellSlice ref;
  try {
    ref = vm::load_cell_slice_special(cs.fetch_ref(), special);
  } catch (...) {
    return td::Status::Error("stonfi payment: ref slice");
  }
  TRY_RESULT(amount0, load_coins_py(ref));
  (void)amount0;
  TRY_RESULT(token0, load_address_py(ref));
  (void)token0;
  TRY_RESULT(amount1, load_coins_py(ref));
  (void)amount1;
  TRY_RESULT(token1, load_address_py(ref));
  (void)token1;
  return exit_code;
}

// --- stonfi v2 --------------------------------------------------------------

// Open a ref cell into a CellSlice (abort-safe).
td::Result<vm::CellSlice> open_ref(const td::Ref<vm::Cell> &c) {
  bool special = false;
  try {
    return vm::load_cell_slice_special(c, special);
  } catch (...) {
    return td::Status::Error("ref slice");
  }
}

// messages/swaps.py StonfiV2PayTo. Fields the core reads: exit_code,
// amount0_out, token0_address, amount1_out, token1_address (canonical upper).
struct PayTo {
  std::uint32_t exit_code{0};
  td::RefInt256 amount0_out, amount1_out;
  Value token0, token1;  // account values
};
td::Result<PayTo> parse_pay_to(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64) || !cs.advance(32 + 64)) {
    return td::Status::Error("pay_to: header underflow");
  }
  TRY_RESULT(to_addr, load_address_py(cs));
  (void)to_addr;
  TRY_RESULT(exc, load_address_py(cs));
  (void)exc;
  TRY_RESULT(orig, load_address_py(cs));
  (void)orig;
  if (!cs.have(32 + 1)) {
    return td::Status::Error("pay_to: exit_code underflow");
  }
  PayTo pt;
  pt.exit_code = static_cast<std::uint32_t>(cs.fetch_ulong(32));
  // custom_payload maybe-ref.
  if (cs.fetch_ulong(1)) {
    if (cs.size_refs() < 1) {
      return td::Status::Error("pay_to: custom_payload ref missing");
    }
    cs.fetch_ref();
  }
  if (cs.size_refs() < 1) {
    return td::Status::Error("pay_to: additional_info ref missing");
  }
  TRY_RESULT(add, open_ref(cs.fetch_ref()));
  auto &ai = add;
  TRY_RESULT(fwd_ton, load_coins_py(ai));
  (void)fwd_ton;
  TRY_RESULT_ASSIGN(pt.amount0_out, load_coins_py(ai));
  TRY_RESULT_ASSIGN(pt.token0, load_address_py(ai));
  TRY_RESULT_ASSIGN(pt.amount1_out, load_coins_py(ai));
  TRY_RESULT_ASSIGN(pt.token1, load_address_py(ai));
  return pt;
}

// messages/swaps.py StonfiSwapV2: token_wallet1 + custom_payload cell (2 ref
// levels deep). The other fields are not read by the core.
struct SwapV2 {
  Value token_wallet1;             // account
  td::Ref<vm::Cell> custom_payload;  // null when absent
};
td::Result<SwapV2> parse_swap_v2(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64) || !cs.advance(32 + 64)) {
    return td::Status::Error("swap_v2: header underflow");
  }
  TRY_RESULT(from_user, load_address_py(cs));
  (void)from_user;
  TRY_RESULT(left, load_coins_py(cs));
  (void)left;
  TRY_RESULT(right, load_coins_py(cs));
  (void)right;
  if (cs.size_refs() < 1) {
    return td::Status::Error("swap_v2: dex_payload ref missing");
  }
  TRY_RESULT(dex, open_ref(cs.fetch_ref()));
  if (!dex.have(32) || !dex.advance(32)) {  // transferred_op
    return td::Status::Error("swap_v2: transferred_op underflow");
  }
  SwapV2 out;
  TRY_RESULT_ASSIGN(out.token_wallet1, load_address_py(dex));
  TRY_RESULT(refund, load_address_py(dex));
  (void)refund;
  TRY_RESULT(exc, load_address_py(dex));
  (void)exc;
  if (!dex.have(64) || !dex.advance(64)) {  // tx_deadline
    return td::Status::Error("swap_v2: tx_deadline underflow");
  }
  if (dex.size_refs() < 1) {
    return td::Status::Error("swap_v2: swap_body ref missing");
  }
  TRY_RESULT(sb, open_ref(dex.fetch_ref()));
  TRY_RESULT(min_out, load_coins_py(sb));
  (void)min_out;
  TRY_RESULT(receiver, load_address_py(sb));
  (void)receiver;
  TRY_RESULT(fwd_gas, load_coins_py(sb));
  (void)fwd_gas;
  // custom_payload maybe-ref (first maybe-ref in swap_body).
  if (!sb.have(1)) {
    return td::Status::Error("swap_v2: custom_payload bit underflow");
  }
  if (sb.fetch_ulong(1)) {
    if (sb.size_refs() < 1) {
      return td::Status::Error("swap_v2: custom_payload ref missing");
    }
    out.custom_payload = sb.fetch_ref();
  }
  return out;
}

// StonfiSwapV2.get_pool_accounts_recursive: walk the custom_payload cross-swap
// chain, collecting pool wallet addresses (canonical upper; "" where a decoded
// address is addr_none). Faults on a malformed head (Python load_uint underflow
// raises), which propagates to a match rejection.
td::Result<std::vector<std::string>> pool_accounts(const SwapV2 &sv) {
  std::vector<std::string> accounts;
  accounts.push_back(acc_str(sv.token_wallet1).value_or(std::string{}));
  if (sv.custom_payload.is_null()) {
    return accounts;
  }
  TRY_RESULT(cur, open_ref(sv.custom_payload));
  while (true) {
    if (!cur.have(32)) {
      return td::Status::Error("pool_accounts: sum_type underflow");
    }
    auto sum_type = static_cast<std::uint32_t>(cur.fetch_ulong(32));
    if (sum_type != kV2Swap && sum_type != kV2CrossSwapB) {
      break;
    }
    TRY_RESULT(addr, load_address_py(cur));
    accounts.push_back(acc_str(addr).value_or(std::string{}));
    if (cur.size_refs() == 0) {
      break;
    }
    TRY_RESULT(cross, open_ref(cur.fetch_ref()));
    TRY_RESULT(min_out, load_coins_py(cross));
    (void)min_out;
    TRY_RESULT(c2, load_coins_py(cross));
    (void)c2;
    if (cross.size_refs() == 0) {
      break;
    }
    if (!cross.have(1)) {
      return td::Status::Error("pool_accounts: maybe bit underflow");
    }
    if (cross.fetch_ulong(1)) {
      if (cross.size_refs() < 1) {
        return td::Status::Error("pool_accounts: inner ref missing");
      }
      TRY_RESULT_ASSIGN(cur, open_ref(cross.fetch_ref()));
    } else {
      break;
    }
  }
  return accounts;
}

// address_selectors.py extract_target_wallet_stonfi_v2_swap: the single target
// wallet (canonical upper) from a swap-request body, or nullopt.
std::optional<std::string> extract_target_wallet(const td::Ref<vm::Cell> &body) {
  auto r_ctx = open_body(body);
  if (r_ctx.is_error()) {
    return std::nullopt;
  }
  auto ctx = r_ctx.move_as_ok();
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64) || !cs.advance(32 + 64)) {
    return std::nullopt;
  }
  if (load_coins_py(cs).is_error() || load_address_py(cs).is_error()) {
    return std::nullopt;
  }
  if (!cs.have(1)) {
    return std::nullopt;
  }
  vm::CellSlice payload;
  if (cs.fetch_ulong(1)) {
    if (cs.size_refs() < 1) {
      return std::nullopt;
    }
    auto r = open_ref(cs.fetch_ref());
    if (r.is_error()) {
      return std::nullopt;
    }
    payload = r.move_as_ok();
  } else {
    payload = cs;  // Slice.copy()
  }
  if (payload.size() == 0 || !payload.have(32)) {
    return std::nullopt;
  }
  auto op = static_cast<std::uint32_t>(payload.fetch_ulong(32));
  if (op != kV2Swap) {
    return std::nullopt;
  }
  auto r_addr = load_address_py(payload);
  if (r_addr.is_error()) {
    return std::nullopt;
  }
  return acc_str(r_addr.ok());
}

}  // namespace

// builders/stonfi_v1.py stonfi_v1_sender_payment: parse the payment request
// body (abort-safe) and accept the sender-related exit codes (ok / no_liq /
// reserve_err); the ok_ref referral payment fails this gate and stays
// unconsumed. Any parse failure -> False (Python try/except).
bool stonfi_v1_sender_payment(const Block *block) {
  auto r_body = block_body(block);
  if (r_body.is_error()) {
    return false;
  }
  auto r = stonfi_payment_exit_code(r_body.ok());
  if (r.is_error()) {
    return false;
  }
  std::uint32_t ec = r.ok();
  return ec == kStonfiSwapOk || ec == kStonfiSwapNoLiq || ec == kStonfiSwapReserveErr;
}

// builders/stonfi_v1.py _stonfi_v1_swap_data -> blocks/swaps.py
// build_stonfi_v1_swap_core. Under the IR engine this fn ALWAYS rejects: the
// core resolves the outgoing transfer via
// `next(b for b in payment_request_block.next_blocks
//       if isinstance(b, JettonTransferBlock))` — JettonTransferBlock is the
// LEGACY class, and every produced jetton_transfer is now the generic Block,
// so the generator is empty and `next(...)` with no default raises
// StopIteration on the first sender-related payment request. (If no payment
// request were sender-related the loop would leave out_addr None and
// `out_addr.to_str()` would raise instead.) Either path is an unconditional
// exception -> EvalError -> match rejection, matching the Python twin's 0
// actions for this matcher. Wired for surface parity (the SKIP->included flip
// adds zero ACTIONs).
EvalResult stonfi_v1_swap_data(BuildEnv &, const std::vector<Value> &args) {
  if (args.size() != 2) {
    return rt_fault("stonfi_v1_swap_data: bad arguments");
  }
  return rt_fault("stonfi_v1_swap_data: JettonTransferBlock legacy class absent "
                  "under IR (StopIteration)");
}

// builders/stonfi.py no_internal_transfer: not block.data['has_internal_transfer'].
bool no_internal_transfer(const Block *block) {
  Value v = data_field(block, "has_internal_transfer");
  bool hit = (v.t == VType::Bool && v.boolean) ||
             (v.t == VType::Int && !v.num.is_null() && v.num->sgn() != 0);
  return !hit;
}

// builders/stonfi.py pton_self_transfer -> validate_stonfi_v2_intermediate_pton_transfer
// (try/except -> False). The intermediate pton call's previous jetton_transfer
// must be non-internal, address the same receiver, and the pton forward payload
// must carry the swap opcode.
bool pton_self_transfer(const Block *block) {
  const Block *prev = block->previous_block;
  if (prev == nullptr) {
    return false;
  }
  Value hit = data_field(prev, "has_internal_transfer");
  if ((hit.t == VType::Bool && hit.boolean) ||
      (hit.t == VType::Int && !hit.num.is_null() && hit.num->sgn() != 0)) {
    return false;
  }
  Value recv = data_field(prev, "receiver");
  const Message *m = block_msg(block);
  std::optional<std::string> dst = m != nullptr ? m->destination : std::nullopt;
  Value dst_acc = account_from_opt(dst);
  // AccountId(receiver) != message.destination -> canonical compare.
  auto rs = acc_str(recv), ds = acc_str(dst_acc);
  if (rs != ds) {
    return false;
  }
  auto r_body = block_body(block);
  if (r_body.is_error()) {
    return false;
  }
  auto r = parse_message_body("PTonTransfer", r_body.ok());
  if (r.is_error()) {
    return false;
  }
  const Value *fp = r.ok().field("forward_payload");
  if (fp == nullptr || fp->t != VType::Cell) {
    return false;
  }
  auto r_cs = open_ref(fp->cell);
  if (r_cs.is_error()) {
    return false;
  }
  auto cs = r_cs.move_as_ok();
  if (!cs.have(32)) {
    return false;
  }
  return static_cast<std::uint32_t>(cs.fetch_ulong(32)) == kV2Swap;
}

namespace {

// The bigger-amount (amount, token) of a pay_to's two out-legs (assets.sort by
// amount desc, take [0]; stable on ties -> token0).
std::pair<td::RefInt256, Value> bigger_leg(const PayTo &pt) {
  bool zero0 = pt.amount0_out.is_null();
  bool zero1 = pt.amount1_out.is_null();
  int cmp = (zero0 || zero1) ? 0 : td::cmp(pt.amount0_out, pt.amount1_out);
  if (cmp >= 0) {
    return {pt.amount0_out, pt.token0};
  }
  return {pt.amount1_out, pt.token1};
}

// dict {amount, asset}.
Value leg_dict(Value amount, Value asset) {
  Value::Fields f;
  f.emplace_back("amount", std::move(amount));
  f.emplace_back("asset", std::move(asset));
  return Value::make_dict(std::move(f));
}

Value asset_from_wallet_jetton(const Value &wallet) {
  const Value *jf = wallet.is_null() ? nullptr : wallet.field("jetton");
  if (jf == nullptr || jf->t != VType::Str) {
    return Value::null();  // wallet.jetton None: caller treats as non-pton null
  }
  if (is_pton_master(jf->str)) {
    return Value::make_asset_ton();
  }
  auto norm = normalize_raw_address(jf->str);
  return Value::make_asset_jetton(norm ? *norm : jf->str);
}

}  // namespace

// builders/stonfi.py _stonfi_v2_swap_data -> blocks/swaps.py
// build_stonfi_v2_swap_core. No try/except in the Python wrapper, so any parse/
// nav failure raises -> match rejection (rt_fault here). NOTE the dead isinstance
// at swaps.py:404: in_transfer is a generic jetton_transfer Block or a
// PTonTransfer call, never the LEGACY JettonTransferBlock, so the JettonTransfer
// arm is DEAD — the in-leg is always synthesized from the message (asset TON),
// reproduced here.
EvalResult stonfi_v2_swap_data(BuildEnv &env, const std::vector<Value> &args) {
  if (args.size() != 1 || args[0].t != VType::List || args[0].items->empty()) {
    return rt_fault("stonfi_v2_swap_data: bad arguments");
  }
  std::vector<const Block *> consumed;
  for (const Value &v : *args[0].items) {
    const Block *b = as_block(v);
    if (b != nullptr) {
      consumed.push_back(b);
    }
  }
  if (consumed.empty()) {
    return rt_fault("stonfi_v2_swap_data: empty consumed");
  }
  const Block *anchor = consumed.front();

  // --- _derive_parts ---
  std::vector<const Block *> payouts;
  for (const Block *b : consumed) {
    if (is_call_op(b, kV2PayTo)) {
      payouts.push_back(b);
    }
  }
  auto in_payouts = [&](const Block *p) {
    return std::find(payouts.begin(), payouts.end(), p) != payouts.end();
  };
  std::vector<const Block *> swaps;
  for (const Block *b : consumed) {
    if (is_call_op(b, kV2Swap)) {
      swaps.push_back(b);
    }
  }
  std::stable_sort(swaps.begin(), swaps.end(),
                   [](const Block *a, const Block *b) { return a->min_lt < b->min_lt; });
  std::vector<std::pair<const Block *, const Block *>> peer_swap_blocks;
  for (const Block *s : swaps) {
    const Block *pay = nullptr;
    for (Block *p : s->next_blocks) {
      if (in_payouts(p)) {
        pay = p;
        break;
      }
    }
    if (pay == nullptr) {
      return rt_fault("stonfi_v2: swap without payout child");  // next() no default
    }
    peer_swap_blocks.emplace_back(s, pay);
  }
  const Block *prev = anchor->previous_block;
  if (prev == nullptr) {
    return rt_fault("stonfi_v2: anchor has no previous block");
  }
  const Block *in_transfer =
      prev->btype == "jetton_transfer" ? prev : prev->previous_block;
  const Block *out_transfer = nullptr;
  std::int64_t best_lt = 0;
  for (const Block *b : consumed) {
    if (b->btype == "jetton_transfer" && in_payouts(b->previous_block)) {
      if (out_transfer == nullptr || b->min_lt > best_lt) {
        out_transfer = b;
        best_lt = b->min_lt;
      }
    }
  }

  // --- build_stonfi_v2_swap_core ---
  bool ok = true;
  struct Step {
    Value token_wallet1;
    std::int64_t min_lt;
    PayTo pay_to;
    SwapV2 swap;
  };
  std::vector<Step> steps;
  for (auto &pr : peer_swap_blocks) {
    auto rb_pay = block_body(pr.second);
    if (rb_pay.is_error()) {
      return rt_fault("stonfi_v2: pay_to body");
    }
    auto r_pt = parse_pay_to(rb_pay.move_as_ok());
    if (r_pt.is_error()) {
      return rt_fault("stonfi_v2: pay_to parse");
    }
    PayTo pt = r_pt.move_as_ok();
    if (pt.exit_code != kStonfiSwapOk) {
      ok = false;
    }
    auto rb_sw = block_body(pr.first);
    if (rb_sw.is_error()) {
      return rt_fault("stonfi_v2: swap body");
    }
    auto r_sv = parse_swap_v2(rb_sw.move_as_ok());
    if (r_sv.is_error()) {
      return rt_fault("stonfi_v2: swap parse");
    }
    SwapV2 sv = r_sv.move_as_ok();
    steps.push_back({sv.token_wallet1, pr.first->min_lt, std::move(pt), std::move(sv)});
  }
  std::stable_sort(steps.begin(), steps.end(),
                   [](const Step &a, const Step &b) { return a.min_lt < b.min_lt; });

  // target pool asset (only observed via the failed-swap fallback).
  auto r_accts = pool_accounts(steps.front().swap);
  if (r_accts.is_error()) {
    return rt_fault("stonfi_v2: pool accounts");
  }
  std::vector<std::string> accts = r_accts.move_as_ok();
  Value destination_asset = Value::null();
  {
    const std::string &target = accts.back();
    Value tpw = env.lookups->get("jetton_wallet", std::vector<Value>{Value::make_str(target)});
    if (!tpw.is_null()) {
      destination_asset = asset_from_wallet_jetton(tpw);  // null when wallet.jetton null
    }
  }

  // per-hop pool wallet -> asset map.
  std::map<std::string, Value> pool_map;
  for (const Step &st : steps) {
    auto key = acc_str(st.token_wallet1);
    if (!key) {
      return rt_fault("stonfi_v2: step wallet addr_none");  // .to_str on None
    }
    Value jw = env.lookups->get("jetton_wallet", std::vector<Value>{Value::make_str(*key)});
    if (!jw.is_null()) {
      pool_map[*key] = asset_from_wallet_jetton(jw);
    } else {
      break;  // block.broken = True (unobserved); stop filling the map
    }
  }

  // in-transfer (§11.5 dual dispatch, deterministic). in_transfer is EITHER a
  // produced jetton_transfer block (btype "jetton_transfer" — read its data
  // dict) OR a PTonTransfer call (the message-synthesized TON leg). Python
  // swaps.py:404 now gates on btype too, so the else arm handles ONLY the
  // single-leaf pTON call — no more hash-seed-dependent event_nodes[0] read.
  if (in_transfer == nullptr) {
    return rt_fault("stonfi_v2: no in_transfer");
  }
  Value sender;
  Value in_data;
  if (in_transfer->btype == "jetton_transfer") {
    Value asset_v = data_field(in_transfer, "asset");
    // asset.jetton_address.as_str(): a TON asset (jetton_address None) would
    // AttributeError before the pton test -> reject.
    if (asset_v.t != VType::Asset || asset_v.is_ton) {
      return rt_fault("stonfi_v2: in asset has no jetton_address");
    }
    Value in_asset = is_pton_master(asset_v.str) ? Value::make_asset_ton() : asset_v;
    sender = data_field(in_transfer, "sender");
    in_data = Value::make_dict(Value::Fields{
        {"asset", std::move(in_asset)},
        {"amount", data_field(in_transfer, "amount")},
        {"source", data_field(in_transfer, "sender")},
        {"source_jetton_wallet", data_field(in_transfer, "sender_wallet")},
        {"destination", data_field(in_transfer, "receiver")},
        {"destination_jetton_wallet", data_field(in_transfer, "receiver_wallet")}});
  } else {
    const Message *im = block_msg(in_transfer);
    if (im == nullptr) {
      return rt_fault("stonfi_v2: in_transfer has no message");
    }
    Value in_amount;
    if (im->opcode32() && *im->opcode32() == kPTonTransfer) {
      auto rb = block_body(in_transfer);
      if (rb.is_error()) {
        return rt_fault("stonfi_v2: pton in body");
      }
      auto r = parse_message_body("PTonTransfer", rb.ok());
      if (r.is_error()) {
        return rt_fault("stonfi_v2: pton in parse");
      }
      const Value *ta = r.ok().field("ton_amount");
      in_amount = ta != nullptr ? to_amount(*ta) : Value::make_amount_none();
    } else {
      in_amount = im->value ? Value::make_amount(td::make_refint(*im->value))
                            : Value::make_amount_none();
    }
    sender = account_from_opt(im->source);
    const Message *anchor_msg = block_msg(anchor);
    in_data = Value::make_dict(Value::Fields{
        {"asset", Value::make_asset_ton()},
        {"amount", std::move(in_amount)},
        {"source", account_from_opt(im->source)},
        {"source_jetton_wallet", Value::null()},
        {"destination",
         account_from_opt(anchor_msg != nullptr ? anchor_msg->source : std::nullopt)},
        {"destination_jetton_wallet", account_from_opt(im->destination)}});
  }

  // peer_swaps (multi-hop only).
  std::vector<Value> peer_swaps;
  auto map_asset = [&](const Value &tok) -> const Value * {
    auto k = acc_str(tok);
    if (!k) {
      return nullptr;
    }
    auto it = pool_map.find(*k);
    return it == pool_map.end() ? nullptr : &it->second;
  };
  if (steps.size() > 1) {
    auto big = bigger_leg(steps[0].pay_to);
    const Value *oa = map_asset(big.second);
    if (oa == nullptr) {
      return rt_fault("stonfi_v2: peer out asset missing");  // KeyError / addr_none
    }
    Value out_leg = leg_dict(Value::make_amount(big.first.is_null() ? td::make_refint(0)
                                                                     : big.first),
                             *oa);
    const Value *ia = in_data.field("amount");
    const Value *ias = in_data.field("asset");
    peer_swaps.push_back(Value::make_dict(Value::Fields{
        {"in", leg_dict(ia != nullptr ? *ia : Value::null(),
                        ias != nullptr ? *ias : Value::null())},
        {"out", out_leg}}));
    if (steps[0].pay_to.exit_code == kStonfiSwapOk) {
      for (std::size_t i = 0; i + 1 < steps.size(); i++) {
        const PayTo &pti = steps[i + 1].pay_to;
        auto b2 = bigger_leg(pti);
        if (pti.exit_code != kStonfiSwapOk) {
          continue;
        }
        const Value *oa2 = map_asset(b2.second);
        if (oa2 == nullptr) {
          return rt_fault("stonfi_v2: peer out asset missing (hop)");
        }
        const Value *prev_out = peer_swaps.back().field("out");
        Value out2 = leg_dict(Value::make_amount(b2.first.is_null() ? td::make_refint(0)
                                                                    : b2.first),
                              *oa2);
        peer_swaps.push_back(Value::make_dict(Value::Fields{
            {"in", prev_out != nullptr ? *prev_out : Value::null()}, {"out", out2}}));
      }
    }
  }

  // out-transfer.
  if (out_transfer == nullptr) {
    return rt_fault("stonfi_v2: no out_transfer");  // out_transfer.next_blocks
  }
  const Block *pton_transfer = nullptr;
  for (Block *n : out_transfer->next_blocks) {
    if (is_call_op(n, kPTonTransfer)) {
      pton_transfer = n;
      break;
    }
  }
  Value has_int = data_field(out_transfer, "has_internal_transfer");
  bool out_has_internal = (has_int.t == VType::Bool && has_int.boolean) ||
                          (has_int.t == VType::Int && !has_int.num.is_null() &&
                           has_int.num->sgn() != 0);
  Value out_data;
  if (pton_transfer == nullptr && out_has_internal) {
    Value asset_f = data_field(out_transfer, "asset");
    // asset.jetton_address.as_str(): a TON asset has jetton_address None ->
    // AttributeError -> reject. Only the non-pton jetton path is live here.
    if (!(asset_f.t == VType::Asset) || asset_f.is_ton) {
      return rt_fault("stonfi_v2: out asset has no jetton_address");
    }
    Value out_asset = is_pton_master(asset_f.str) ? Value::make_asset_ton() : asset_f;
    out_data = Value::make_dict(Value::Fields{
        {"asset", std::move(out_asset)},
        {"amount", data_field(out_transfer, "amount")},
        {"source", data_field(out_transfer, "sender")},
        {"source_jetton_wallet", data_field(out_transfer, "sender_wallet")},
        {"destination", data_field(out_transfer, "receiver")},
        {"destination_jetton_wallet", data_field(out_transfer, "receiver_wallet")}});
  } else {
    if (pton_transfer == nullptr) {
      return rt_fault("stonfi_v2: pton out missing");  // PTonTransfer(None) -> raise
    }
    auto rb = block_body(pton_transfer);
    if (rb.is_error()) {
      return rt_fault("stonfi_v2: pton out body");
    }
    auto r = parse_message_body("PTonTransfer", rb.ok());
    if (r.is_error()) {
      return rt_fault("stonfi_v2: pton out parse");
    }
    const Value *ta = r.ok().field("ton_amount");
    const Message *pm = block_msg(pton_transfer);
    out_data = Value::make_dict(Value::Fields{
        {"asset", Value::make_asset_ton()},
        {"amount", ta != nullptr ? to_amount(*ta) : Value::make_amount_none()},
        {"source", data_field(out_transfer, "sender")},
        {"source_jetton_wallet", data_field(out_transfer, "sender_wallet")},
        {"destination", account_from_opt(pm != nullptr ? pm->destination : std::nullopt)},
        {"destination_jetton_wallet", Value::null()}});
  }

  const Value *src_asset = in_data.field("asset");
  Value dst_asset = [&]() {
    const Value *a = out_data.field("asset");
    return a != nullptr ? *a : Value::null();
  }();

  // failed-swap destination_asset override.
  if (!ok) {
    if (!destination_asset.is_null()) {
      dst_asset = destination_asset;
    } else if (prev != nullptr) {
      // _get_stonfi_v2_target_asset_from_notification(anchor.previous_block msg)
      auto rb = block_body(prev);
      if (!rb.is_error()) {
        auto tw = extract_target_wallet(rb.ok());
        if (tw) {
          Value jw =
              env.lookups->get("jetton_wallet", std::vector<Value>{Value::make_str(*tw)});
          if (!jw.is_null()) {
            const Value *jf = jw.field("jetton");
            if (jf != nullptr && jf->t == VType::Str) {
              auto norm = normalize_raw_address(jf->str);
              dst_asset = Value::make_asset_jetton(norm ? *norm : jf->str);
            }
          }
        }
      }
    }
  }

  Value::Fields d;
  d.emplace_back("failed", Value::make_bool(!ok));
  d.emplace_back("dex", Value::make_str("stonfi_v2"));
  d.emplace_back("source_asset", src_asset != nullptr ? *src_asset : Value::null());
  d.emplace_back("destination_asset", std::move(dst_asset));
  d.emplace_back("sender", std::move(sender));
  d.emplace_back("dex_incoming_transfer", std::move(in_data));
  d.emplace_back("dex_outgoing_transfer", std::move(out_data));
  d.emplace_back("referral_amount", Value::null());
  d.emplace_back("referral_address", Value::null());
  d.emplace_back("peer_swaps",
                 Value::make_list(peer_swaps.size() > 1 ? peer_swaps : std::vector<Value>{}));
  return rt_ok(Value::make_obj(std::move(d)));
}

}  // namespace mch
