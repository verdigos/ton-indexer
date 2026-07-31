// Host binding registries and dispatch (Phase D). The per-protocol predicate
// and fn implementations live in src/host/*; this file owns the host_fns() /
// host_predicates() maps and the rt_call_hostfn dispatch.
#include "HostRegistry.h"

#include "BuildRuntime.h"
#include "host/HostImpls.h"

namespace mch {

const std::map<std::string, HostFn> &host_fns() {
  static const std::map<std::string, HostFn> fns = {
      {"dedust_swap_legs", dedust_swap_legs},
      {"dedust_swap_failed", dedust_swap_failed},
      {"dedust_v2_swap_payload", dedust_v2_swap_payload},
      {"dedust_v2_deposit_payload", dedust_v2_deposit_payload},
      {"layerzero_dst_oapp_matches", layerzero_dst_oapp_matches},
      {"tonstakers_minted_nft", tonstakers_minted_nft},
      {"tonstakers_pool_addr", tonstakers_pool_addr},
      {"vesting_message_was_sent", vesting_message_was_sent},
      {"jvault_stake_period", jvault_stake_period},
      {"nft_transfer_data", nft_transfer_data},
      {"getgems_nft_purchase_data", getgems_nft_purchase_data},
      {"telegram_nft_purchase_data", telegram_nft_purchase_data},
      {"is_teleitem", is_teleitem},
      {"tgbtc_mint_data", tgbtc_mint_data},
      {"coffee_swap_data", coffee_swap_data},
      {"stonfi_v1_swap_data", stonfi_v1_swap_data},
      {"dedust_deposit_final_data", dedust_deposit_final_data},
      {"dedust_deposit_partial_data", dedust_deposit_partial_data},
      {"tonco_deposit_liquidity_data", tonco_deposit_liquidity_data},
      {"tonco_withdraw_payouts", tonco_withdraw_payouts},
      {"evaa_supply_data", evaa_supply_data},
      {"tonco_swap_data", tonco_swap_data},
      // stonfi_v2_swap_data: registered by the §11.5 dual-dispatch wave. The
      // Python core (build_stonfi_v2_swap_core) now reads the in-leg by btype
      // (swaps.py:404) instead of the hash-seed-dependent
      // in_transfer.event_nodes[0].message, so it is deterministic and matches
      // this already-btype-driven port. A/B parity is now achievable.
      {"stonfi_v2_swap_data", stonfi_v2_swap_data},
      // specs/nft_sale.mch — GetGems StateInit readers, the chained-lookup bid
      // builder, and the cancel/finish switch discriminator.
      {"getgems_sale_init", getgems_sale_init},
      {"getgems_auction_init", getgems_auction_init},
      {"auction_bid_data", auction_bid_data},
      {"nft_trade_is_finish", nft_trade_is_finish},
      {"nft_trade_returned", nft_trade_returned},
      {"auction_outbid_data", auction_outbid_data},
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
  EvalResult r = it->second(env, args);
  if (r.faulted) {
    reject_log(r.message);
  }
  return r;
}

const std::map<std::string, HostPredFn> &host_predicates() {
  static const std::map<std::string, HostPredFn> preds = {
      {"nominator_pool_withdraw_parent", nominator_pool_withdraw_parent},
      {"evaa_user_withdraw_user", evaa_user_withdraw_user},
      {"evaa_user_withdraw_success", evaa_user_withdraw_success},
      {"evaa_user_withdraw_fail", evaa_user_withdraw_fail},
      {"evaa_service_comment", evaa_service_comment},
      {"evaa_supply_anchor", evaa_supply_anchor},
      {"evaa_user_supply", evaa_user_supply},
      {"getgems_purchase", getgems_purchase},
      {"getgems_seller_payout", getgems_seller_payout},
      // specs/nft_mint.mch's pred ANCHOR (no opcode/btype prefilter, spec R10).
      {"single_contract_deploy", single_contract_deploy},
      {"stonfi_v1_sender_payment", stonfi_v1_sender_payment},
      // pton_self_transfer gates stonfi_v2_swap. (no_internal_transfer folded
      // into an inline where-expr in W6.)
      {"pton_self_transfer", pton_self_transfer},
      // specs/nft_sale.mch.
      {"sale_contract_deploy", sale_contract_deploy},
      {"nft_trade_cancel_comment", nft_trade_cancel_comment},
      {"nft_trade_finish_comment", nft_trade_finish_comment},
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
