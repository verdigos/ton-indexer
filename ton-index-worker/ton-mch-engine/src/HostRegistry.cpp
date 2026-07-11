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
      // stonfi_v2_swap_data: implemented (HostStonfi.cpp) but NOT registered.
      // The Python reference core (build_stonfi_v2_swap_core) is non-
      // deterministic on the in-leg: under the IR engine in_transfer is a
      // generic Block (isinstance JettonTransferBlock False), so swaps.py:419's
      // else arm reads in_transfer.event_nodes[0].message, and the produced
      // jetton_transfer's event_nodes order is hash-seed dependent -> the twin's
      // dex_incoming_transfer flakes run-to-run. A/B parity is impossible until
      // the Python dual-dispatch wave makes that arm btype-driven/deterministic.
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
      // no_internal_transfer / pton_self_transfer implemented (HostStonfi.cpp)
      // but NOT registered: they gate only stonfi_v2_swap, which is disabled
      // (see the host_fns() note on stonfi_v2_swap_data).
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
