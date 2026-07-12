// --dexrecords-test: typed host-authoring seam unit tests (see DexRecordsTest.h).
#include "DexRecordsTest.h"

#include "BlockTree.h"
#include "BuildRuntime.h"
#include "ExprRuntime.h"
#include "Value.h"
#include "host/BlockViews.h"
#include "host/DexPton.h"
#include "host/DexRecords.h"
#include "host/HostAdapter.h"

#include <cstdio>
#include <string>
#include <vector>

namespace mch {

namespace {

int g_fail = 0;

void check(const std::string &name, bool ok) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
  if (!ok) {
    g_fail++;
  }
}

// Nth field of a Dict/Obj has key `k`.
bool key_at(const Value &d, std::size_t i, const char *k) {
  return d.fields && i < d.fields->size() && d.fields->at(i).first == k;
}
const Value *val_at(const Value &d, std::size_t i) {
  return (d.fields && i < d.fields->size()) ? &d.fields->at(i).second : nullptr;
}

Value jetton(const std::string &master) { return Value::make_asset_jetton(master); }

// --- encoder shapes ---------------------------------------------------------

void test_transfer_leg_shape() {
  TransferLeg leg;
  leg.asset = Value::make_asset_ton();
  leg.amount = Value::make_amount(td::make_refint(5));
  leg.source = Value::make_account_none();
  leg.source_jetton_wallet = Value::null();
  leg.destination = Value::make_account_none();
  leg.destination_jetton_wallet = Value::null();
  Value d = leg.encode();
  check("leg/is_dict", d.t == VType::Dict && d.fields && d.fields->size() == 6);
  check("leg/order",
        key_at(d, 0, "asset") && key_at(d, 1, "amount") && key_at(d, 2, "source") &&
            key_at(d, 3, "source_jetton_wallet") && key_at(d, 4, "destination") &&
            key_at(d, 5, "destination_jetton_wallet"));
  const Value *a = val_at(d, 0);
  check("leg/asset_value", a != nullptr && a->t == VType::Asset && a->is_ton);
}

void test_swap_leg_shape() {
  SwapLeg leg;
  leg.asset = jetton("0:AB");
  leg.amount = Value::make_amount(td::make_refint(7));
  Value d = leg.encode();
  check("swapleg/order",
        d.t == VType::Dict && d.fields && d.fields->size() == 2 && key_at(d, 0, "asset") &&
            key_at(d, 1, "amount"));
}

void test_peer_swap_shape() {
  PeerSwap p;
  p.in.asset = Value::make_asset_ton();
  p.in.amount = Value::make_amount(td::make_refint(1));
  p.out.asset = jetton("0:CD");
  p.out.amount = Value::make_amount(td::make_refint(2));
  Value d = p.encode();
  check("peer/order",
        d.t == VType::Dict && d.fields && d.fields->size() == 2 && key_at(d, 0, "in") &&
            key_at(d, 1, "out"));
  const Value *in = val_at(d, 0);
  check("peer/in_is_swapleg", in != nullptr && in->t == VType::Dict && key_at(*in, 0, "asset"));
}

SwapRecord sample_record() {
  SwapRecord rec;
  rec.dex = "coffee";
  rec.sender = Value::make_account_none();
  rec.source_asset = Value::make_asset_ton();
  rec.destination_asset = jetton("0:EF");
  rec.dex_incoming_transfer.asset = Value::make_asset_ton();
  rec.dex_outgoing_transfer.asset = jetton("0:EF");
  rec.referral_amount = Value::null();
  rec.referral_address = Value::null();
  rec.failed = false;
  return rec;
}

void test_record_shape() {
  SwapRecord rec = sample_record();
  Value d = rec.encode();
  check("record/is_obj", d.t == VType::Obj && d.fields && d.fields->size() == 10);
  check("record/order",
        key_at(d, 0, "dex") && key_at(d, 1, "sender") && key_at(d, 2, "source_asset") &&
            key_at(d, 3, "destination_asset") && key_at(d, 4, "dex_incoming_transfer") &&
            key_at(d, 5, "dex_outgoing_transfer") && key_at(d, 6, "peer_swaps") &&
            key_at(d, 7, "referral_amount") && key_at(d, 8, "referral_address") &&
            key_at(d, 9, "failed"));
}

void test_record_peer_gate() {
  // 0 hops -> peer_swaps is an empty list.
  SwapRecord rec = sample_record();
  Value d0 = rec.encode();
  const Value *ps0 = val_at(d0, 6);
  check("record/peer_empty_zero", ps0 != nullptr && ps0->t == VType::List && ps0->items->empty());

  // 1 hop -> STILL an empty list (Coffee's `size > 1` convention).
  PeerSwap p;
  p.in.asset = Value::make_asset_ton();
  p.in.amount = Value::make_amount(td::make_refint(1));
  p.out.asset = jetton("0:EF");
  p.out.amount = Value::make_amount(td::make_refint(2));
  rec.peer_swaps = {p};
  Value d1 = rec.encode();
  const Value *ps1 = val_at(d1, 6);
  check("record/peer_empty_one", ps1 != nullptr && ps1->t == VType::List && ps1->items->empty());

  // 2 hops -> rendered.
  rec.peer_swaps = {p, p};
  Value d2 = rec.encode();
  const Value *ps2 = val_at(d2, 6);
  check("record/peer_rendered_two", ps2 != nullptr && ps2->t == VType::List &&
                                        ps2->items->size() == 2);
}

void test_fill_peer_out_assets() {
  SwapRecord rec = sample_record();
  rec.dex_outgoing_transfer.asset = jetton("0:FINAL");
  PeerSwap h0, h1;
  h0.in.asset = Value::make_asset_ton();
  h0.out.asset = Value::null();
  h1.in.asset = jetton("0:MID");
  h1.out.asset = Value::null();
  rec.peer_swaps = {h0, h1};
  rec.fill_peer_out_assets();
  // hop0.out.asset == hop1.in.asset (the mid asset); last.out.asset == outgoing asset.
  bool mid = rec.peer_swaps[0].out.asset.t == VType::Asset &&
             rec.peer_swaps[0].out.asset.str == "0:MID";
  bool fin = rec.peer_swaps[1].out.asset.t == VType::Asset &&
             rec.peer_swaps[1].out.asset.str == "0:FINAL";
  check("fill/mid_from_next_in", mid);
  check("fill/last_from_outgoing", fin);
}

// --- adapter arity/reject/fault policy --------------------------------------

// A block-list arg wrapping the given blocks.
Value consumed_arg(const std::vector<const Block *> &blocks) {
  std::vector<Value> xs;
  for (const Block *b : blocks) {
    xs.push_back(Value::make_block(b));
  }
  return Value::make_list(std::move(xs));
}

void test_decode_consumed() {
  BlockArena arena;
  Block *b = arena.make("jetton_transfer");
  ConsumedBlocks out;
  // arity: zero args -> fault.
  check("decode/zero_args_fault", decode_consumed({}, "t", out).faulted);
  // wrong type -> fault.
  check("decode/non_list_fault",
        decode_consumed({Value::make_str("x")}, "t", out).faulted);
  // empty list -> fault.
  check("decode/empty_list_fault",
        decode_consumed({Value::make_list({})}, "t", out).faulted);
  // list of non-blocks -> "empty consumed" fault.
  check("decode/non_block_fault",
        decode_consumed({Value::make_list({Value::make_str("x")})}, "t", out).faulted);
  // valid -> ok, anchor + others derived.
  EvalResult ok = decode_consumed({consumed_arg({b, b})}, "t", out);
  check("decode/valid_ok", !ok.faulted && out.blocks.size() == 2 && out.anchor() == b &&
                               out.others().size() == 1);
}

// Test cores for run_swap_host.
HostResult<SwapRecord> core_reject(HostContext &, const ConsumedBlocks &) {
  return HostResult<SwapRecord>::reject();
}
HostResult<SwapRecord> core_fault(HostContext &, const ConsumedBlocks &) {
  return HostResult<SwapRecord>::fault("boom");
}
HostResult<SwapRecord> core_ok(HostContext &, const ConsumedBlocks &) {
  return HostResult<SwapRecord>::ok(sample_record());
}

void test_run_swap_host() {
  BlockArena arena;
  Block *b = arena.make("jetton_transfer");
  NullLookupTable lookups;
  BuildEnv env;
  env.lookups = &lookups;
  std::vector<Value> args = {consumed_arg({b})};

  // bad args propagate the decode fault.
  check("run/bad_args_fault", run_swap_host(env, {}, "t", core_reject).faulted);
  // reject -> rt_ok(Null).
  EvalResult rj = run_swap_host(env, args, "t", core_reject);
  check("run/reject_is_null", !rj.faulted && rj.value.is_null());
  // fault -> faulted.
  check("run/fault_faulted", run_swap_host(env, args, "t", core_fault).faulted);
  // ok -> encoded Obj.
  EvalResult ok = run_swap_host(env, args, "t", core_ok);
  check("run/ok_is_obj", !ok.faulted && ok.value.t == VType::Obj);
}

// --- pTON wallet->asset helper ----------------------------------------------

Value wallet_with_jetton(const std::string &master) {
  return Value::make_obj(Value::Fields{{"jetton", Value::make_str(master)}});
}

void test_pton_helper() {
  const std::string &pton = pton_masters().front();
  const std::string other = "0:1234";

  check("pton/is_master_true", is_pton_master(pton));
  check("pton/is_master_false", !is_pton_master(other));

  // null wallet: convert -> Null; no-convert -> Asset(TON).
  check("pton/null_convert", wallet_jetton_asset(Value::null(), true).is_null());
  Value nc = wallet_jetton_asset(Value::null(), false);
  check("pton/null_noconvert_ton", nc.t == VType::Asset && nc.is_ton);

  // pton-master wallet: convert -> TON; no-convert -> jetton asset (v1).
  Value pc = wallet_jetton_asset(wallet_with_jetton(pton), true);
  check("pton/master_convert_ton", pc.t == VType::Asset && pc.is_ton);
  Value pn = wallet_jetton_asset(wallet_with_jetton(pton), false);
  check("pton/master_noconvert_jetton", pn.t == VType::Asset && pn.has_jetton);

  // ordinary jetton wallet: jetton asset either way.
  Value oc = wallet_jetton_asset(wallet_with_jetton(other), true);
  check("pton/other_convert_jetton", oc.t == VType::Asset && oc.has_jetton);
}

// --- block views (W4) -------------------------------------------------------

void test_data_truthy() {
  BlockArena arena;
  Block *b = arena.make("jetton_transfer");
  b->data = Value::make_dict(Value::Fields{
      {"flag_true", Value::make_bool(true)},
      {"flag_false", Value::make_bool(false)},
      {"one", Value::make_int(td::make_refint(1))},
      {"zero", Value::make_int(td::make_refint(0))},
      {"null", Value::null()}});
  check("view/data_truthy_bool_true", data_truthy(b, "flag_true"));
  check("view/data_truthy_bool_false", !data_truthy(b, "flag_false"));
  check("view/data_truthy_int_nonzero", data_truthy(b, "one"));
  check("view/data_truthy_int_zero", !data_truthy(b, "zero"));
  check("view/data_truthy_null", !data_truthy(b, "null"));
  check("view/data_truthy_absent", !data_truthy(b, "missing"));
}

void test_acc_str() {
  const std::string addr = "0:" + std::string(64, 'A');
  auto s = acc_str(Value::make_account_raw(addr));
  check("view/acc_str_account", s.has_value() && *s == addr);
  check("view/acc_str_none", !acc_str(Value::make_account_none()).has_value());
  check("view/acc_str_nonaccount", !acc_str(Value::make_str(addr)).has_value());
}

}  // namespace

int run_dexrecords_test() {
  g_fail = 0;
  test_transfer_leg_shape();
  test_swap_leg_shape();
  test_peer_swap_shape();
  test_record_shape();
  test_record_peer_gate();
  test_fill_peer_out_assets();
  test_decode_consumed();
  test_run_swap_host();
  test_pton_helper();
  test_data_truthy();
  test_acc_str();
  std::printf("DEXRECORDS-TEST %s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
  return g_fail == 0 ? 0 : 1;
}

}  // namespace mch
