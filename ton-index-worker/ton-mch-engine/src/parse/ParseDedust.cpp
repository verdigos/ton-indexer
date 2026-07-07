// DeDust message parsers (messages/swaps.py + blocks/swaps.py). See
// parse/PSlice.h for shared machinery and MsgParse.cpp's header for the
// pytoniq-parity catalogue.
#include "parse/Parsers.h"

#include "MsgParse.h"
#include "parse/PSlice.h"

#include "common/refint.h"
#include "vm/cellslice.h"

#include <string>
#include <utility>
#include <vector>

namespace mch {

// DedustPayoutFromPool (messages/swaps.py).
td::Result<Value> parse_dedust_payout_from_pool(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64) || !cs.advance(32)) {
    return td::Status::Error("dedust payout: header underflow");
  }
  auto query_id = cs.fetch_ulong(64);
  if (cs.size_refs() == 0) {
    return td::Status::Error("dedust payout: proof ref missing");
  }
  td::Ref<vm::Cell> proof = cs.fetch_ref();
  TRY_RESULT(amount, load_coins_py(cs));
  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(query_id)));
  f.emplace_back("proof", Value::make_cell(std::move(proof)));
  f.emplace_back("amount", Value::make_int(std::move(amount)));
  return Value::make_obj(std::move(f));
}

namespace {

// messages/swaps.py load_asset: kind:uint4 (0 -> TON) else wc:uint8 + 32 bytes.
// Asset(False, Address((wc, bytes))) -> canonical "wc:HEX" upper (AccountId).
td::Result<Value> load_dedust_asset(vm::CellSlice &cs) {
  if (!cs.have(4)) {
    return td::Status::Error("dedust asset: kind underflow");
  }
  auto kind = cs.fetch_ulong(4);
  if (kind == 0) {
    return Value::make_asset_ton();
  }
  if (!cs.have(8 + 256)) {
    return td::Status::Error("dedust asset: jetton underflow");
  }
  int wc = static_cast<int>(cs.fetch_ulong(8));
  unsigned char buf[32];
  if (!cs.fetch_bytes(buf, 32)) {
    return td::Status::Error("dedust asset: hash fetch failed");
  }
  return Value::make_asset_jetton(std::to_string(wc) + ":" + hex_upper(buf, 32));
}

}  // namespace

// DedustSwapNotification (messages/swaps.py). Parses the full body incl. the
// ref (Python load_ref() raises when absent -> parse fails -> host fn rejects).
td::Result<Value> parse_dedust_swap_notification(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32) || !cs.advance(32)) {
    return td::Status::Error("dedust notify: opcode underflow");
  }
  TRY_RESULT(asset_in, load_dedust_asset(cs));
  TRY_RESULT(asset_out, load_dedust_asset(cs));
  TRY_RESULT(amount_in, load_coins_py(cs));
  TRY_RESULT(amount_out, load_coins_py(cs));
  if (cs.size_refs() == 0) {
    return td::Status::Error("dedust notify: ref missing");
  }
  bool special = false;
  vm::CellSlice ref;
  try {
    ref = vm::load_cell_slice_special(cs.fetch_ref(), special);
  } catch (...) {
    return td::Status::Error("dedust notify: bad ref cell");
  }
  TRY_RESULT(sender_address, load_address_py(ref));
  TRY_RESULT(ref_address, load_address_py(ref));
  TRY_RESULT(reserve_0, load_coins_py(ref));
  TRY_RESULT(reserve_1, load_coins_py(ref));
  Value::Fields f;
  f.emplace_back("asset_in", std::move(asset_in));
  f.emplace_back("asset_out", std::move(asset_out));
  f.emplace_back("amount_in", Value::make_int(std::move(amount_in)));
  f.emplace_back("amount_out", Value::make_int(std::move(amount_out)));
  f.emplace_back("sender_address", std::move(sender_address));
  f.emplace_back("ref_address", std::move(ref_address));
  f.emplace_back("reserve_0", Value::make_int(std::move(reserve_0)));
  f.emplace_back("reserve_1", Value::make_int(std::move(reserve_1)));
  return Value::make_obj(std::move(f));
}

// blocks/swaps.py _parse_dedust_steps: walk the SwapStep chain (already
// positioned past the sum-type/header). Each link: pool addr, 1 flag bit,
// coins, maybe-ref to the next step. Returns the pool addresses in walk order
// (AccountId(load_address()).as_str(); addr_none -> "addr_none").
td::Result<std::vector<std::string>> parse_dedust_steps(vm::CellSlice cs) {
  std::vector<std::string> steps;
  for (;;) {
    TRY_RESULT(addr, load_address_py(cs));  // pytoniq load_address
    std::string pool = (addr.t == VType::Account && !addr.addr_none) ? addr.str : "addr_none";
    if (!cs.have(1)) {
      return td::Status::Error("dedust steps: flag bit underflow");
    }
    cs.advance(1);                    // load_bit
    TRY_RESULT(_coins, load_coins_py(cs));
    (void)_coins;
    if (!cs.have(1)) {                // load_maybe_ref: the Maybe bit
      return td::Status::Error("dedust steps: maybe-ref bit underflow");
    }
    bool has_next = cs.fetch_ulong(1) != 0;
    steps.push_back(std::move(pool));
    if (!has_next) {
      return steps;
    }
    if (cs.size_refs() == 0) {
      return td::Status::Error("dedust steps: next-step ref missing");
    }
    bool special = false;
    try {
      cs = vm::load_cell_slice_special(cs.fetch_ref(), special);  // next_step.to_slice()
    } catch (...) {
      return td::Status::Error("dedust steps: bad next-step cell");
    }
  }
}

}  // namespace mch
