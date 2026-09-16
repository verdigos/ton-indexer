// Host binding registries and dispatch. Per-protocol predicate
// and fn implementations live in src/host/*; this file owns the host_fns() /
// host_predicates() maps and the rt_call_hostfn dispatch.
#include "HostRegistry.h"

#include "BuildRuntime.h"
#include "host/HostImpls.h"

namespace mch {

const std::map<std::string, HostFnEntry> &host_fns() {
  static const std::map<std::string, HostFnEntry> fns = {
      {"dedust_swap_legs", {dedust_swap_legs, 3}},
      {"dedust_v2_swap_min_out", {dedust_v2_swap_min_out, 1}},
      {"tonstakers_pool_addr", {tonstakers_pool_addr, 1}},
      {"vesting_message_was_sent", {vesting_message_was_sent, 2}},
      {"nft_transfer_data", {nft_transfer_data, 3}},
      {"telegram_nft_purchase_data", {telegram_nft_purchase_data, 3}},
      {"tgbtc_mint_data", {tgbtc_mint_data, 1}},
      {"stonfi_v1_swap_data", {stonfi_v1_swap_data, 2}},
      {"dedust_deposit_final_data", {dedust_deposit_final_data, 1}},
      {"dedust_deposit_partial_data", {dedust_deposit_partial_data, 1}},
      {"tonco_deposit_liquidity_data", {tonco_deposit_liquidity_data, 1}},
      {"tonco_withdraw_payouts", {tonco_withdraw_payouts, 1}},
      {"tonco_swap_data", {tonco_swap_data, 1}},
      // stonfi_v2_swap_data reads the in-leg by block type instead of a
      // hash-seed-dependent first event node, so it is deterministic.
      {"stonfi_v2_swap_data", {stonfi_v2_swap_data, 1}},
      // GetGems StateInit readers and chained-lookup bid builder.
      {"getgems_sale_init", {getgems_sale_init, 2}},
      {"getgems_auction_init", {getgems_auction_init, 2}},
      {"auction_bid_data", {auction_bid_data, 1}},
  };
  return fns;
}

EvalResult rt_call_hostfn(BuildEnv &env, const std::string &name,
                          const std::vector<Value> &args) {
  auto it = host_fns().find(name);
  if (it == host_fns().end()) {
    return rt_fault("unknown host fn '" + name + "'");
  }
  // Name the fn (and its anchor) for the [mch-reject] line: the fault below and
  // any host_reject() inside the fn both read them off the context.
  RejectCtx ctx = reject_ctx();
  ctx.fn = &name;
  ctx.anchor = env.anchor;
  RejectScope scope(ctx);
  if (args.size() != static_cast<std::size_t>(it->second.arity)) {
    EvalResult r = rt_fault(name + ": bad arguments");
    reject_log(r.message);
    return r;
  }
  EvalResult r = it->second.fn(env, args);
  if (r.faulted) {
    reject_log(r.message);
  }
  return r;
}

const std::map<std::string, HostPredFn> &host_predicates() {
  static const std::map<std::string, HostPredFn> preds = {
      {"evaa_supply_anchor", evaa_supply_anchor},
      {"evaa_liquidate_anchor", evaa_liquidate_anchor},
      // Predicate anchor has no opcode or btype prefilter.
      {"single_contract_deploy", single_contract_deploy},
      // pton_self_transfer gates stonfi_v2_swap; no_internal_transfer is an
      // inline where expression.
      {"pton_self_transfer", pton_self_transfer},
      {"sale_contract_deploy", sale_contract_deploy},
      {"auction_bid_candidate", auction_bid_candidate},
      {"auction_outbid_leg", auction_outbid_leg},
  };
  return preds;
}

const std::map<std::string, HostShaperFn> &host_shapers() {
  static const std::map<std::string, HostShaperFn> shapers = {
      {"getgems_proxy_insert", getgems_proxy_insert},
      {"nft_transfer_parent_absorb", nft_transfer_parent_absorb},
      {"tgbtc_deploy_absorb", tgbtc_deploy_absorb},
  };
  return shapers;
}

}  // namespace mch
