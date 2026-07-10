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
  };
  return preds;
}

const std::map<std::string, HostShaperFn> &host_shapers() {
  static const std::map<std::string, HostShaperFn> shapers = {
      {"getgems_proxy_insert", getgems_proxy_insert},
  };
  return shapers;
}

}  // namespace mch
