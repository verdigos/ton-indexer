// Internal host-layer surface: the registered host predicates and fns, one TU
// per protocol, plus the post-pass spine surgery ClassifyCore runs after the
// walk. Declared here so HostRegistry.cpp's host_predicates()/host_fns() maps
// and ClassifyCore.cpp's run_post_processors can reference them across TU
// boundaries. NOT part of the public HostRegistry.h surface.
#pragma once

#include "ExprRuntime.h"
#include "Value.h"

#include <set>
#include <vector>

namespace mch {

struct Block;       // BlockTree.h
struct BlockArena;  // BlockTree.h
struct BuildEnv;    // BuildRuntime.h
struct ShaperMatch;  // HostRegistry.h

// Getgems (HostNftSale.cpp): finish/stop sibling-payout shaper.
void getgems_proxy_insert(Block *produced, const ShaperMatch &m);

// NFT (HostNft.cpp): metadata projection, telegram soft-parse+payouts, parent-absorb.
EvalResult nft_transfer_data(BuildEnv &env, const std::vector<Value> &args);
void nft_transfer_parent_absorb(Block *produced, const ShaperMatch &m);
EvalResult telegram_nft_purchase_data(BuildEnv &env, const std::vector<Value> &args);
// Exactly one distinct contract deployment across the block's event nodes.
bool single_contract_deploy(const Block *block);

// GetGems sale/auction family (HostNftSale.cpp).
bool sale_contract_deploy(const Block *block);

// tgBTC (HostTgbtc.cpp): mint host fn and deploy-absorb shaper.
EvalResult tgbtc_mint_data(BuildEnv &env, const std::vector<Value> &args);
void tgbtc_deploy_absorb(Block *produced, const ShaperMatch &m);

// Stonfi (HostStonfi.cpp): v1/v2 swap fns.
EvalResult stonfi_v1_swap_data(BuildEnv &env, const std::vector<Value> &args);
EvalResult stonfi_v2_swap_data(BuildEnv &env, const std::vector<Value> &args);

// pTON (DexPton.cpp): TON-wrapper master predicate.
EvalResult is_pton_master(BuildEnv &env, const std::vector<Value> &args);

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

// Post-pass spine surgery for the liquidity-deposit pairs, in the order
// ClassifyCore's run_post_processors fixes: DeDust V1 merge, then DeDust V2
// pairing, then STON.fi V2 pairing. `removed` collects the folded-in blocks.
void post_process_dedust_liquidity(std::vector<Block *> &blocks, std::set<Block *> &removed);
void post_process_dedust_v2_liquidity(std::vector<Block *> &blocks, std::set<Block *> &removed);
void post_process_stonfi_v2_liquidity(std::vector<Block *> &blocks, BlockArena &arena,
                                      std::set<Block *> &removed);
}  // namespace mch
