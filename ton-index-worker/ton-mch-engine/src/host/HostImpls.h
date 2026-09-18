// Internal registry surface: the registered host predicates and fns, one TU per
// protocol. Declared here so
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

// Getgems (HostNftSale.cpp): finish/stop sibling-payout shaper.
void getgems_proxy_insert(Block *produced, const ShaperMatch &m);

// NFT (HostNft.cpp): base nft_transfer host fn + parent-absorb shaper + telegram.
EvalResult nft_transfer_data(BuildEnv &env, const std::vector<Value> &args);
void nft_transfer_parent_absorb(Block *produced, const ShaperMatch &m);
EvalResult telegram_nft_purchase_data(BuildEnv &env, const std::vector<Value> &args);
// Exactly one distinct contract deployment across the block's event nodes.
bool single_contract_deploy(const Block *block);

// GetGems sale/auction family (HostNftSale.cpp).
bool sale_contract_deploy(const Block *block);
EvalResult getgems_sale_init(BuildEnv &env, const std::vector<Value> &args);
EvalResult getgems_auction_init(BuildEnv &env, const std::vector<Value> &args);

// tgBTC (HostTgbtc.cpp): mint host fn and deploy-absorb shaper.
EvalResult tgbtc_mint_data(BuildEnv &env, const std::vector<Value> &args);
void tgbtc_deploy_absorb(Block *produced, const ShaperMatch &m);

// Stonfi (HostStonfi.cpp): v1/v2 swap fns.
EvalResult stonfi_v1_swap_data(BuildEnv &env, const std::vector<Value> &args);
EvalResult stonfi_v2_swap_data(BuildEnv &env, const std::vector<Value> &args);

// Tonco (HostTonco.cpp): deposit-liquidity, withdraw-payout, and swap fns.
EvalResult tonco_deposit_liquidity_data(BuildEnv &env, const std::vector<Value> &args);
EvalResult tonco_withdraw_payouts(BuildEnv &env, const std::vector<Value> &args);
EvalResult tonco_swap_data(BuildEnv &env, const std::vector<Value> &args);

// DeDust (HostDedust.cpp).
EvalResult dedust_swap_legs(BuildEnv &env, const std::vector<Value> &args);
EvalResult dedust_v2_swap_min_out(BuildEnv &env, const std::vector<Value> &args);

// DeDust liquidity deposit (HostDedustDeposit.cpp).
EvalResult dedust_deposit_final_data(BuildEnv &env, const std::vector<Value> &args);
EvalResult dedust_deposit_partial_data(BuildEnv &env, const std::vector<Value> &args);

// Tonstakers (HostTonstakers.cpp).
EvalResult tonstakers_pool_addr(BuildEnv &env, const std::vector<Value> &args);

// Vesting (HostVesting.cpp).
EvalResult vesting_message_was_sent(BuildEnv &env, const std::vector<Value> &args);

}  // namespace mch
