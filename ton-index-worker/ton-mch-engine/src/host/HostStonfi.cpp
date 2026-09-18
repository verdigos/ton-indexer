#include "host/HostImpls.h"

#include "host/BlockViews.h"
#include "host/DexPton.h"
#include "host/DexRecords.h"
#include "host/HostAdapter.h"
#include "host/HostCommon.h"

#include "BlockTree.h"
#include "BuildRuntime.h"
#include "ExprRuntime.h"
#include "MsgParse.h"
#include "parse/PSlice.h"
#include "btypes_gen.h"

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
constexpr std::uint32_t kStonfiSwap = 0x25938561;      // v1 swap request call
constexpr std::uint32_t kStonfiSwapOk = 0xc64370e5;
constexpr std::uint32_t kStonfiSwapOkRef = 0x45078540;  // ok_ref (referral leg)
constexpr std::uint32_t kStonfiSwapNoLiq = 0x5ffe1295;
constexpr std::uint32_t kStonfiSwapReserveErr = 0x38976e9b;

// Swap-request call is 0x6664de2a (also the forward-payload swap opcode and a
// cross-swap sum-type tag); pay_to is 0x657b54f5. The swap body's own opcode
// field is unused and must not be treated as the discriminator.
constexpr std::uint32_t kV2Swap = 0x6664de2a;       // swap request call
constexpr std::uint32_t kV2PayTo = 0x657b54f5;      // pay_to call
constexpr std::uint32_t kV2CrossSwapB = 0x69cf1a5b;
constexpr std::uint32_t kPTonTransfer = 0x01f3835d;
// Only used to say, in a [mch-reject] reason, whether an absent out leg was a
// jetton transfer that never became a jetton_transfer BLOCK (its own matcher
// rejects on a missing wallet interface) rather than an absent message.
constexpr std::uint32_t kJettonTransfer = 0x0f8a7ea5;

// Full field set must parse before exit_code is reported: a failure past
// exit_code (e.g. a missing ref) must still reject. Atomic parse-then-report
// is intentional.
struct PaymentReq {
  std::uint32_t exit_code{0};
  td::RefInt256 amount0_out, amount1_out;
  Value token0, token1;  // account values
};
// Unpack the parsed payment-request object; parse failure is a Status.
td::Result<PaymentReq> parse_payment_request(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(v, parse_message_body("StonfiPaymentRequest", body));
  const Value *info_cell = v.field("info");
  const Value *info = info_cell != nullptr ? info_cell->field("ref") : nullptr;
  if (info == nullptr) {
    return td::Status::Error("payment request: info.ref missing");
  }
  PaymentReq pr;
  pr.exit_code = static_cast<std::uint32_t>(v.field("exit_code")->num->to_long());
  pr.amount0_out = info->field("amount0_out")->num;
  pr.token0 = *info->field("token0");
  pr.amount1_out = info->field("amount1_out")->num;
  pr.token1 = *info->field("token1");
  return pr;
}

bool sender_related(std::uint32_t ec) {
  return ec == kStonfiSwapOk || ec == kStonfiSwapNoLiq || ec == kStonfiSwapReserveErr;
}

// Pay-to fields the core reads: exit_code, amount0_out, token0_address,
// amount1_out, token1_address (canonical upper).
struct PayTo {
  std::uint32_t exit_code{0};
  td::RefInt256 amount0_out, amount1_out;
  Value token0, token1;  // account values
};
td::Result<PayTo> parse_pay_to(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(v, parse_message_body("StonfiV2PayTo", body));
  const Value *info_cell = v.field("info");
  const Value *info = info_cell != nullptr ? info_cell->field("ref") : nullptr;
  if (info == nullptr) {
    return td::Status::Error("stonfi pay_to: info.ref missing");
  }
  PayTo pt;
  pt.exit_code = static_cast<std::uint32_t>(v.field("exit_code")->num->to_long());
  pt.amount0_out = info->field("amount0_out")->num;
  pt.token0 = *info->field("token0");
  pt.amount1_out = info->field("amount1_out")->num;
  pt.token1 = *info->field("token1");
  return pt;
}

// v2 swap: token_wallet1 + custom_payload cell (2 ref levels deep). Other
// fields are unread.
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
  TRY_RESULT(dex, open_ref_cell(cs.fetch_ref()));
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
  TRY_RESULT(sb, open_ref_cell(dex.fetch_ref()));
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

// Walk the custom_payload cross-swap chain for pool wallet addresses
// (canonical upper; "" for addr_none). A malformed head faults and rejects.
td::Result<std::vector<std::string>> pool_accounts(const SwapV2 &sv) {
  std::vector<std::string> accounts;
  accounts.push_back(acc_str(sv.token_wallet1).value_or(std::string{}));
  if (sv.custom_payload.is_null()) {
    return accounts;
  }
  TRY_RESULT(cur, open_ref_cell(sv.custom_payload));
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
    TRY_RESULT(cross, open_ref_cell(cur.fetch_ref()));
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
      TRY_RESULT_ASSIGN(cur, open_ref_cell(cross.fetch_ref()));
    } else {
      break;
    }
  }
  return accounts;
}

// Single target wallet (canonical upper) from a swap-request body, or nullopt.
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
    auto r = open_ref_cell(cs.fetch_ref());
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

// Args are the in_transfer anchor and StonfiSwap call. Resolve the outgoing
// transfer by btype. Any nav/parse/lookup miss is rt_fault (match rejection).
EvalResult stonfi_v1_swap_data(BuildEnv &env, const std::vector<Value> &args) {
  const Block *swap = as_block(args[1]);
  if (swap == nullptr) {
    return rt_fault("stonfi_v1: null capture");
  }
  (void)env;

  // Read payment requests with a jetton transfer child in tree order.
  struct PR { PaymentReq msg; const Block *blk; };
  std::vector<PR> prs;
  for (Block *x : swap->next_blocks) {
    if (!is_call_op(x, kStonfiPaymentRequest)) continue;
    bool has_jt = false;
    for (Block *nb : x->next_blocks) {
      if (nb->btype == mch::btype::kJettonTransfer) { has_jt = true; break; }
    }
    if (!has_jt) continue;
    auto rb = block_body(x);
    if (rb.is_error()) return rt_fault("stonfi_v1: payment body");
    auto rp = parse_payment_request(rb.move_as_ok());
    if (rp.is_error()) return rt_fault("stonfi_v1: payment parse");
    prs.push_back({rp.move_as_ok(), x});
  }
  if (prs.empty()) return rt_fault("stonfi_v1: no payment requests");

  auto first_jt_child = [](const Block *pb) -> const Block * {
    for (Block *b : pb->next_blocks) {
      if (b->btype == mch::btype::kJettonTransfer) return b;
    }
    return nullptr;
  };

  td::RefInt256 out_amt, ref_amt;   // null == absent
  Value out_addr, ref_addr;         // Null == absent
  const Block *outgoing_jt = nullptr;
  bool success_swap = false;

  for (auto &pr : prs) {
    td::RefInt256 amount;
    Value addr;
    if (!pr.msg.amount0_out.is_null() && pr.msg.amount0_out->sgn() > 0) {
      amount = pr.msg.amount0_out;
      addr = pr.msg.token0;
    } else {
      amount = pr.msg.amount1_out;
      addr = pr.msg.token1;
    }
    if (sender_related(pr.msg.exit_code)) {
      success_swap = (pr.msg.exit_code == kStonfiSwapOk);
      if (out_amt.is_null()) {
        outgoing_jt = first_jt_child(pr.blk);
        out_amt = amount;
        out_addr = addr;
      } else if (td::cmp(out_amt, amount) < 0) {
        outgoing_jt = first_jt_child(pr.blk);
        ref_amt = out_amt;
        ref_addr = out_addr;
        out_amt = amount;
        out_addr = addr;
      }
    } else if (pr.msg.exit_code == kStonfiSwapOkRef) {
      ref_amt = amount;
      ref_addr = addr;  // referral_request_blocks dropped (never consumed)
    }
  }

  if (outgoing_jt == nullptr) {
    return rt_fault("stonfi_v1: no outgoing jetton transfer (payment_requests=" +
                    std::to_string(prs.size()) + ")");
  }

  Value::Fields d;
  d.emplace_back("outgoing_transfer", Value::make_block(outgoing_jt));
  d.emplace_back("out_amount", amount_or_zero(out_amt));
  d.emplace_back("out_address", std::move(out_addr));
  d.emplace_back("referral_amount",
                 ref_amt.is_null() ? Value::make_amount_none() : Value::make_amount(ref_amt));
  d.emplace_back("referral_address", std::move(ref_addr));
  d.emplace_back("success", Value::make_bool(success_swap));
  return rt_ok(Value::make_obj(std::move(d)));
}

// The stonfi_v2 jetton-transfer output arm uses the inline predicate
// `where (not .has_internal_transfer)`.

namespace {

// The bigger-amount (amount, token) of a pay_to's two out-legs (assets.sort by
// amount desc, take [0]; stable on ties -> token0).
std::pair<td::RefInt256, Value> bigger_leg(const PayTo &pt) {
  int cmp = td::cmp(pt.amount0_out, pt.amount1_out);
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

// wallet.jetton -> Asset with the pTON conversion (a missing jetton yields Null,
// the caller treats it as "no asset").
Value asset_from_wallet_jetton(const Value &wallet) {
  return wallet_jetton_asset(wallet, /*pton_conversion=*/true);
}

}  // namespace

// Any parse/nav failure is rt_fault (match rejection). Incoming btype selects
// the leg: jetton_transfer reads produced-block data; the pTON call path
// synthesizes the message-backed TON leg.
EvalResult stonfi_v2_swap_data(BuildEnv &env, const std::vector<Value> &args) {
  ConsumedBlocks decoded;
  EvalResult decode = decode_consumed(args, "stonfi_v2_swap_data", decoded);
  if (decode.faulted) {
    return decode;
  }
  std::vector<const Block *> consumed = std::move(decoded.blocks);
  const Block *anchor = consumed.front();

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
      return rt_fault("stonfi_v2: swap without payout child");
    }
    peer_swap_blocks.emplace_back(s, pay);
  }
  const Block *prev = anchor->previous_block;
  if (prev == nullptr) {
    return rt_fault("stonfi_v2: anchor has no previous block");
  }
  const Block *in_transfer =
      prev->btype == mch::btype::kJettonTransfer ? prev : prev->previous_block;
  const Block *out_transfer = nullptr;
  std::int64_t best_lt = 0;
  for (const Block *b : consumed) {
    if (b->btype == mch::btype::kJettonTransfer && in_payouts(b->previous_block)) {
      if (out_transfer == nullptr || b->min_lt > best_lt) {
        out_transfer = b;
        best_lt = b->min_lt;
      }
    }
  }

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
      return rt_fault("stonfi_v2: step wallet addr_none");
    }
    Value jw = env.lookups->get("jetton_wallet", std::vector<Value>{Value::make_str(*key)});
    if (!jw.is_null()) {
      pool_map[*key] = asset_from_wallet_jetton(jw);
    } else {
      break;  // unobserved broken-block case: stop filling the map
    }
  }

  // Dispatch by incoming btype: a produced jetton_transfer supplies its data;
  // the single-leaf PTonTransfer call synthesizes the message-backed TON leg.
  if (in_transfer == nullptr) {
    return rt_fault("stonfi_v2: no in_transfer");
  }
  Value sender;
  Value in_data;
  Value in_asset_value;
  Value in_amount_value;
  if (in_transfer->btype == mch::btype::kJettonTransfer) {
    Value asset_v = data_field(in_transfer, "asset");
    // A TON or absent in-asset rejects: the in-leg wallet carried no master.
    if (asset_v.t != VType::Asset || asset_v.is_ton) {
      return rt_fault("stonfi_v2: in asset has no jetton_address (asset=" + asset_v.describe() +
                      " receiver_wallet=" +
                      acc_str(data_field(in_transfer, "receiver_wallet")).value_or("none") + ")");
    }
    in_asset_value = is_pton_master(asset_v.str) ? Value::make_asset_ton() : asset_v;
    in_amount_value = data_field(in_transfer, "amount");
    sender = data_field(in_transfer, "sender");
    TransferLeg leg = TransferLeg::from_jetton_transfer(in_transfer);
    leg.asset = in_asset_value;  // pton-normalized asset overrides the data field
    in_data = leg.encode();
  } else {
    const Message *im = block_msg(in_transfer);
    if (im == nullptr) {
      return rt_fault("stonfi_v2: in_transfer has no message");
    }
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
      in_amount_value = ta != nullptr ? to_amount(*ta) : Value::make_amount_none();
    } else {
      in_amount_value = msg_value_amount(im);
    }
    in_asset_value = Value::make_asset_ton();
    sender = account_from_opt(im->source);
    const Message *anchor_msg = block_msg(anchor);
    in_data = Value::make_dict(Value::Fields{
        {"asset", in_asset_value},
        {"amount", in_amount_value},
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
      // pool_map is short of `steps` when a per-hop jetton_wallet lookup
      // missed and truncated the fill loop above.
      return rt_fault("stonfi_v2: peer out asset missing (jetton_wallet=" +
                      acc_str(big.second).value_or("none") + " pool_map=" +
                      std::to_string(pool_map.size()) + "/" + std::to_string(steps.size()) + ")");
    }
    Value out_leg = leg_dict(amount_or_zero(big.first), *oa);
    peer_swaps.push_back(Value::make_dict(Value::Fields{
        {"in", leg_dict(in_amount_value, in_asset_value)},
        {"out", out_leg}}));
    Value previous_out = out_leg;
    if (steps[0].pay_to.exit_code == kStonfiSwapOk) {
      for (std::size_t i = 0; i + 1 < steps.size(); i++) {
        const PayTo &pti = steps[i + 1].pay_to;
        auto b2 = bigger_leg(pti);
        if (pti.exit_code != kStonfiSwapOk) {
          continue;
        }
        const Value *oa2 = map_asset(b2.second);
        if (oa2 == nullptr) {
          return rt_fault("stonfi_v2: peer out asset missing (hop) (jetton_wallet=" +
                          acc_str(b2.second).value_or("none") + " pool_map=" +
                          std::to_string(pool_map.size()) + "/" + std::to_string(steps.size()) +
                          ")");
        }
        Value out2 = leg_dict(amount_or_zero(b2.first), *oa2);
        peer_swaps.push_back(Value::make_dict(Value::Fields{
            {"in", previous_out}, {"out", out2}}));
        previous_out = std::move(out2);
      }
    }
  }

  if (out_transfer == nullptr) {
    // Found by btype, so a raw JettonTransfer call still in the consumed set
    // means the jetton_transfer matcher rejected it (missing wallet interface)
    // rather than the trace lacking an out leg.
    std::size_t jt_calls = 0;
    for (const Block *b : consumed) {
      if (is_call_op(b, kJettonTransfer)) jt_calls++;
    }
    return rt_fault("stonfi_v2: no out_transfer (consumed=" + std::to_string(consumed.size()) +
                    " steps=" + std::to_string(steps.size()) + " raw_jetton_transfer_calls=" +
                    std::to_string(jt_calls) + ")");
  }
  const Block *pton_transfer = nullptr;
  for (Block *n : out_transfer->next_blocks) {
    if (is_call_op(n, kPTonTransfer)) {
      pton_transfer = n;
      break;
    }
  }
  bool out_has_internal = data_truthy(out_transfer, "has_internal_transfer");
  Value out_data;
  if (pton_transfer == nullptr && out_has_internal) {
    Value asset_f = data_field(out_transfer, "asset");
    // A TON out-asset rejects. Only the non-pTON jetton path is live here.
    if (!(asset_f.t == VType::Asset) || asset_f.is_ton) {
      return rt_fault("stonfi_v2: out asset has no jetton_address (asset=" + asset_f.describe() +
                      " receiver_wallet=" +
                      acc_str(data_field(out_transfer, "receiver_wallet")).value_or("none") + ")");
    }
    Value out_asset = is_pton_master(asset_f.str) ? Value::make_asset_ton() : asset_f;
    TransferLeg leg = TransferLeg::from_jetton_transfer(out_transfer);
    leg.asset = std::move(out_asset);
    out_data = leg.encode();
  } else {
    if (pton_transfer == nullptr) {
      // Missing pTON out: reached only when the out leg is not an
      // internal-transfer jetton_transfer, so name what it was instead.
      return rt_fault("stonfi_v2: pton out missing (out_transfer=" + out_transfer->btype +
                      " has_internal=" + (out_has_internal ? "1" : "0") + ")");
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
    TransferLeg leg;
    leg.asset = Value::make_asset_ton();
    leg.amount = ta != nullptr ? to_amount(*ta) : Value::make_amount_none();
    leg.source = data_field(out_transfer, "sender");
    leg.source_jetton_wallet = data_field(out_transfer, "sender_wallet");
    leg.destination = account_from_opt(pm != nullptr ? pm->destination : std::nullopt);
    leg.destination_jetton_wallet = Value::null();
    out_data = leg.encode();
  }

  Value dst_asset = [&]() {
    const Value *a = out_data.field("asset");
    return a != nullptr ? *a : Value::null();
  }();

  // failed-swap destination_asset override.
  if (!ok) {
    if (!destination_asset.is_null()) {
      dst_asset = destination_asset;
    } else if (prev != nullptr) {
      // Target asset from the notification previous-block body.
      auto rb = block_body(prev);
      if (!rb.is_error()) {
        auto tw = extract_target_wallet(rb.ok());
        if (tw) {
          Value jw =
              env.lookups->get("jetton_wallet", std::vector<Value>{Value::make_str(*tw)});
          if (auto master = wallet_jetton_master_str(jw)) {
            dst_asset = Value::make_asset_jetton(*master);
          }
        }
      }
    }
  }

  Value::Fields d;
  d.emplace_back("failed", Value::make_bool(!ok));
  d.emplace_back("dex", Value::make_str("stonfi_v2"));
  d.emplace_back("source_asset", in_asset_value);
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
