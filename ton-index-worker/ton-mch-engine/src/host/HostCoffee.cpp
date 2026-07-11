// Coffee swap host fn (builders/coffee.py + blocks/swaps.py
// build_coffee_swap_core). See host/HostImpls.h for the internal registry
// surface. Message bodies are parsed inline (the spec parses none of them).
#include "host/HostImpls.h"

#include "host/HostCommon.h"

#include "BlockTree.h"
#include "BuildRuntime.h"
#include "MsgParse.h"
#include "parse/PSlice.h"

#include "common/refint.h"
#include "vm/cellslice.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace mch {

namespace {

constexpr std::uint32_t kCoffeeSwapNative = 0xc0ffee00;
constexpr std::uint32_t kCoffeeSwapInternal = 0xc0ffee20;
constexpr std::uint32_t kCoffeeSwapSuccessfulEvent = 0xc0ffee30;
constexpr std::uint32_t kCoffeePayoutInternal = 0xc0ffee21;
constexpr std::uint32_t kCoffeePayout = 0xc0ffee32;
constexpr std::uint32_t kCoffeeNotification = 0xc0ffee36;

std::string hex_upper_bytes(const unsigned char *p, std::size_t n) {
  static const char *d = "0123456789ABCDEF";
  std::string out(n * 2, '0');
  for (std::size_t i = 0; i < n; i++) {
    out[2 * i] = d[p[i] >> 4];
    out[2 * i + 1] = d[p[i] & 0xF];
  }
  return out;
}

// messages/coffee.py load_coffee_asset. Advances cs. Errors on extra$10 (Python
// raises NotImplementedError) and the invalid tag.
td::Result<Value> load_coffee_asset(vm::CellSlice &cs) {
  if (!cs.have(2)) {
    return td::Status::Error("asset: underflow");
  }
  auto tag = cs.fetch_ulong(2);
  if (tag == 0) {
    return Value::make_asset_ton();
  }
  if (tag == 1) {
    if (!cs.have(8 + 256)) {
      return td::Status::Error("asset: jetton underflow");
    }
    auto chain = cs.fetch_ulong(8);
    unsigned char hash[32];
    if (!cs.fetch_bytes(hash, 32)) {
      return td::Status::Error("asset: hash fetch");
    }
    std::string master = std::to_string(chain) + ":" + hex_upper_bytes(hash, 32);
    return Value::make_asset_jetton(master);
  }
  return td::Status::Error("asset: unsupported tag");
}

const EventNode *first_node(const Block *b) {
  return (b == nullptr || b->event_nodes.empty()) ? nullptr : b->event_nodes.front();
}

// A leg dict {asset, amount, source, source_jetton_wallet, destination,
// destination_jetton_wallet}.
Value make_leg(Value asset, Value amount, Value source, Value dest) {
  Value::Fields f;
  f.emplace_back("asset", std::move(asset));
  f.emplace_back("amount", std::move(amount));
  f.emplace_back("source", std::move(source));
  f.emplace_back("source_jetton_wallet", Value::null());
  f.emplace_back("destination", std::move(dest));
  f.emplace_back("destination_jetton_wallet", Value::null());
  return Value::make_dict(std::move(f));
}

}  // namespace

// builders/coffee.py _coffee_swap_data + blocks/swaps.py build_coffee_swap_core.
// TON-arm reachable under the IR engine (CoffeeSwapNative in, CoffeePayout /
// CoffeeNotification out); the jetton legs need the skipped jetton_transfer
// block. Returns the {failed, **data} namespace or Null to reject.
EvalResult coffee_swap_data(BuildEnv &, const std::vector<Value> &args) {
  if (args.size() != 1 || args[0].t != VType::List || args[0].items->empty()) {
    return rt_fault("coffee_swap_data: bad arguments");
  }
  std::vector<const Block *> all;
  for (const Value &v : *args[0].items) {
    const Block *b = as_block(v);
    if (b != nullptr) {
      all.push_back(b);
    }
  }
  if (all.empty()) {
    return rt_fault("coffee_swap_data: empty consumed");
  }
  const Block *block = all.front();
  std::vector<const Block *> others(all.begin() + 1, all.end());

  // in_transfer = prev if in others and (jetton_transfer | CoffeeSwapNative).
  const Block *in_transfer = nullptr;
  const Block *prev = block->previous_block;
  if (prev != nullptr) {
    bool in_others = std::find(others.begin(), others.end(), prev) != others.end();
    if (in_others && (prev->btype == "jetton_transfer" || is_call_op(prev, kCoffeeSwapNative))) {
      in_transfer = prev;
    }
  }
  // payout = first CoffeePayoutInternal in others.
  const Block *payout = nullptr;
  for (const Block *b : others) {
    if (is_call_op(b, kCoffeePayoutInternal)) {
      payout = b;
      break;
    }
  }
  // out_transfer = first in others whose previous_block is payout and is a
  // jetton_transfer / CoffeePayout / CoffeeNotification.
  const Block *out_transfer = nullptr;
  if (payout != nullptr) {
    for (const Block *b : others) {
      if (b->previous_block == payout &&
          (b->btype == "jetton_transfer" || is_call_op(b, kCoffeePayout) ||
           is_call_op(b, kCoffeeNotification))) {
        out_transfer = b;
        break;
      }
    }
  }

  // --- build_coffee_swap_core ---
  if (in_transfer == nullptr || payout == nullptr || out_transfer == nullptr) {
    return rt_ok(Value::null());  // not all roles -> reject
  }

  // swap_internal blocks (others + anchor), unique, sorted by min_lt.
  std::vector<const Block *> swaps;
  for (const Block *b : others) {
    if (is_call_op(b, kCoffeeSwapInternal)) {
      swaps.push_back(b);
    }
  }
  swaps.push_back(block);  // the anchor is a CoffeeSwapInternal
  std::sort(swaps.begin(), swaps.end());
  swaps.erase(std::unique(swaps.begin(), swaps.end()), swaps.end());
  std::stable_sort(swaps.begin(), swaps.end(),
                   [](const Block *a, const Block *b) { return a->min_lt < b->min_lt; });

  bool ok = true;
  std::vector<Value> peer_swaps;  // each {in:{asset,amount}, out:{asset,amount}}
  for (const Block *sw : swaps) {
    const Block *event = nullptr;
    for (Block *n : sw->next_blocks) {
      if (is_call_op(n, kCoffeeSwapSuccessfulEvent)) {
        event = n;
        break;
      }
    }
    if (event == nullptr) {
      ok = false;
      continue;
    }
    auto r_body = block_body(event);
    if (r_body.is_error()) {
      return rt_fault("coffee: event body");
    }
    auto r_ctx = open_body(r_body.ok());
    if (r_ctx.is_error()) {
      return rt_fault("coffee: event slice");
    }
    auto ctx = r_ctx.move_as_ok();
    auto &cs = ctx.cs;
    if (!cs.have(32 + 64)) {
      return rt_fault("coffee: event header");
    }
    cs.advance(32 + 64);  // opcode + query_id
    auto r_asset = load_coffee_asset(cs);
    if (r_asset.is_error()) {
      return rt_fault("coffee: event asset");
    }
    auto r_in = load_coins_py(cs);
    auto r_out = load_coins_py(cs);
    if (r_in.is_error() || r_out.is_error()) {
      return rt_fault("coffee: event amounts");
    }
    Value in_leg = Value::make_dict(Value::Fields{
        {"asset", r_asset.move_as_ok()}, {"amount", Value::make_amount(r_in.move_as_ok())}});
    Value out_leg = Value::make_dict(Value::Fields{
        {"asset", Value::null()}, {"amount", Value::make_amount(r_out.move_as_ok())}});
    peer_swaps.push_back(Value::make_dict(Value::Fields{
        {"in", std::move(in_leg)}, {"out", std::move(out_leg)}}));
  }
  if (peer_swaps.empty()) {
    return rt_ok(Value::null());
  }

  // in_transfer_data. The JettonTransferBlock arm of build_coffee_swap_core is
  // isinstance-gated on the LEGACY class; under the IR engine a jetton_transfer
  // is the generic Block, so that branch is dead (in_data stays {}, sender None,
  // matching the Python twin) — only the CoffeeSwapNative arm is live.
  Value sender = Value::null();
  Value in_data = Value::make_dict({});
  if (is_call_op(in_transfer, kCoffeeSwapNative)) {
    auto r_body = block_body(in_transfer);
    if (r_body.is_error()) {
      return rt_fault("coffee: native body");
    }
    auto r_ctx = open_body(r_body.ok());
    if (r_ctx.is_error()) {
      return rt_fault("coffee: native header");
    }
    auto ctx = r_ctx.move_as_ok();
    auto &cs = ctx.cs;
    if (!cs.have(32 + 64)) {
      return rt_fault("coffee: native header");
    }
    cs.advance(32 + 64);
    auto r_amt = load_coins_py(cs);
    if (r_amt.is_error()) {
      return rt_fault("coffee: native amount");
    }
    const Message *m = block_msg(in_transfer);
    sender = account_from_opt(m != nullptr ? m->source : std::nullopt);
    in_data = make_leg(Value::make_asset_ton(), Value::make_amount(r_amt.move_as_ok()), sender,
                       account_from_opt(m != nullptr ? m->destination : std::nullopt));
  }

  // out_transfer_data. The JettonTransferBlock arm is likewise dead under the IR
  // engine (out_data stays {}); only CoffeePayout / CoffeeNotification are live.
  Value out_data = Value::make_dict({});
  if (is_call_op(out_transfer, kCoffeePayout)) {
    auto r_body = block_body(payout);
    if (r_body.is_error()) {
      return rt_fault("coffee: payout body");
    }
    auto r_ctx = open_body(r_body.ok());
    if (r_ctx.is_error()) {
      return rt_fault("coffee: payout header");
    }
    auto ctx = r_ctx.move_as_ok();
    auto &cs = ctx.cs;
    if (!cs.have(32 + 64)) {
      return rt_fault("coffee: payout header");
    }
    cs.advance(32 + 64);
    auto r_rcpt = load_address_py(cs);
    auto r_amt = load_coins_py(cs);
    if (r_rcpt.is_error() || r_amt.is_error()) {
      return rt_fault("coffee: payout fields");
    }
    const Message *m = block_msg(payout);
    out_data = make_leg(Value::make_asset_ton(), Value::make_amount(r_amt.move_as_ok()),
                        account_from_opt(m != nullptr ? m->source : std::nullopt),
                        r_rcpt.move_as_ok());
  } else if (is_call_op(out_transfer, kCoffeeNotification)) {
    const Message *m = block_msg(out_transfer);
    Value amt = (m != nullptr && m->value) ? Value::make_amount(td::make_refint(*m->value))
                                           : Value::make_amount_none();
    out_data = make_leg(Value::make_asset_ton(), std::move(amt),
                        account_from_opt(m != nullptr ? m->source : std::nullopt),
                        account_from_opt(m != nullptr ? m->destination : std::nullopt));
  }

  // Fill peer_swaps out-assets from the next hop / final payout.
  for (std::size_t i = 0; i + 1 < peer_swaps.size(); i++) {
    const Value *next_in = peer_swaps[i + 1].field("in");
    const Value *next_asset = next_in != nullptr ? next_in->field("asset") : nullptr;
    Value *out = const_cast<Value *>(peer_swaps[i].field("out"));
    if (out != nullptr && next_asset != nullptr) {
      out->fields->at(0).second = *next_asset;  // "asset" is first in the out dict
    }
  }
  const Value *out_asset = out_data.field("asset");
  if (out_asset != nullptr) {
    Value *last_out = const_cast<Value *>(peer_swaps.back().field("out"));
    if (last_out != nullptr) {
      last_out->fields->at(0).second = *out_asset;
    }
  }

  const Value *src_asset = in_data.field("asset");
  const Value *dst_asset = out_data.field("asset");

  Value::Fields data;
  data.emplace_back("dex", Value::make_str("coffee"));
  data.emplace_back("sender", sender);
  data.emplace_back("source_asset", src_asset != nullptr ? *src_asset : Value::null());
  data.emplace_back("destination_asset", dst_asset != nullptr ? *dst_asset : Value::null());
  data.emplace_back("dex_incoming_transfer", std::move(in_data));
  data.emplace_back("dex_outgoing_transfer", std::move(out_data));
  data.emplace_back("peer_swaps",
                    Value::make_list(peer_swaps.size() > 1 ? peer_swaps : std::vector<Value>{}));
  data.emplace_back("referral_amount", Value::null());
  data.emplace_back("referral_address", Value::null());
  data.emplace_back("failed", Value::make_bool(!ok));
  return rt_ok(Value::make_obj(std::move(data)));
}

}  // namespace mch
