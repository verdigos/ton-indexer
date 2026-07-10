// Internal registry surface: the registered host predicates and fns, one TU per
// protocol (HostNominator.cpp, HostEvaa.cpp, ...). Declared here so
// HostRegistry.cpp's host_predicates()/host_fns() maps can reference them across
// TU boundaries. NOT part of the public HostRegistry.h surface.
#pragma once

#include "ExprRuntime.h"
#include "Value.h"

#include <vector>

namespace mch {

struct Block;     // BlockTree.h
struct BuildEnv;  // BuildRuntime.h
struct ShaperMatch;  // HostRegistry.h

// Getgems (HostGetgems.cpp): the getgems_proxy_insert shaper only (the match
// predicates land with the matcher's future Python declarative conversion).
void getgems_proxy_insert(Block *produced, const ShaperMatch &m);

// Nominator (HostNominator.cpp).
bool nominator_pool_withdraw_parent(const Block *b);

// EVAA (HostEvaa.cpp).
bool evaa_user_withdraw_user(const Block *b);
bool evaa_user_withdraw_success(const Block *b);
bool evaa_user_withdraw_fail(const Block *b);
bool evaa_service_comment(const Block *b);

// DeDust (HostDedust.cpp).
EvalResult dedust_swap_legs(BuildEnv &env, const std::vector<Value> &args);
EvalResult dedust_swap_failed(BuildEnv &env, const std::vector<Value> &args);

// LayerZero (HostLayerZero.cpp).
EvalResult layerzero_dst_oapp_matches(BuildEnv &env, const std::vector<Value> &args);

// Tonstakers (HostTonstakers.cpp).
EvalResult tonstakers_minted_nft(BuildEnv &env, const std::vector<Value> &args);

// JVault (HostJvault.cpp).
EvalResult jvault_stake_period(BuildEnv &env, const std::vector<Value> &args);
EvalResult jvault_account_list(BuildEnv &env, const std::vector<Value> &args);

}  // namespace mch
