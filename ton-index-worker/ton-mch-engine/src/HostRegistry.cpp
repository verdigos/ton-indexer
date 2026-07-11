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
      {"layerzero_dst_oapp_matches", layerzero_dst_oapp_matches},
      {"tonstakers_minted_nft", tonstakers_minted_nft},
      {"jvault_stake_period", jvault_stake_period},
      {"jvault_account_list", jvault_account_list},
      {"nft_transfer_data", nft_transfer_data},
      {"getgems_nft_purchase_data", getgems_nft_purchase_data},
      {"telegram_nft_purchase_data", telegram_nft_purchase_data},
      {"tgbtc_mint_data", tgbtc_mint_data},
      {"coffee_swap_data", coffee_swap_data},
      {"stonfi_v1_swap_data", stonfi_v1_swap_data},
      {"dedust_deposit_final_data", dedust_deposit_final_data},
      {"dedust_deposit_partial_data", dedust_deposit_partial_data},
      {"tonco_deposit_liquidity_data", tonco_deposit_liquidity_data},
      {"tonco_swap_data", tonco_swap_data},
      // stonfi_v2_swap_data: registered by the §11.5 dual-dispatch wave. The
      // Python core (build_stonfi_v2_swap_core) now reads the in-leg by btype
      // (swaps.py:404) instead of the hash-seed-dependent
      // in_transfer.event_nodes[0].message, so it is deterministic and matches
      // this already-btype-driven port. A/B parity is now achievable.
      {"stonfi_v2_swap_data", stonfi_v2_swap_data},
  };
  return fns;
}

EvalResult rt_call_hostfn(BuildEnv &env, const std::string &name,
                          const std::vector<Value> &args) {
  auto it = host_fns().find(name);
  if (it == host_fns().end()) {
    return rt_fault("unknown host fn '" + name + "'");
  }
  return it->second(env, args);
}

const std::map<std::string, HostPredFn> &host_predicates() {
  static const std::map<std::string, HostPredFn> preds = {
      {"nominator_pool_withdraw_parent", nominator_pool_withdraw_parent},
      {"evaa_user_withdraw_user", evaa_user_withdraw_user},
      {"evaa_user_withdraw_success", evaa_user_withdraw_success},
      {"evaa_user_withdraw_fail", evaa_user_withdraw_fail},
      {"evaa_service_comment", evaa_service_comment},
      {"getgems_purchase", getgems_purchase},
      {"getgems_seller_payout", getgems_seller_payout},
      {"stonfi_v1_sender_payment", stonfi_v1_sender_payment},
      // no_internal_transfer / pton_self_transfer gate stonfi_v2_swap, now
      // enabled by the §11.5 dual-dispatch wave (see host_fns()).
      {"no_internal_transfer", no_internal_transfer},
      {"pton_self_transfer", pton_self_transfer},
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
