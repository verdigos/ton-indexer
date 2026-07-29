// The jvault_assets lookup as raw account-`data` cell parses (W3 §4d).
//
// jvault has no smc-interface detector and needs none: Python's
// extract_jvault_assets (blocks/jvault.py:36-69) never calls a get-method, it
// reads the stake wallet's data cell, then the staking pool's, then one jetton
// wallet. Only the two cell parses live here; the account reads and the jetton
// wallet step belong to the caller (EmuCelldbLookup), which owns the budget and
// the memo.
//
// Celldb is strictly better than kvrocks as the source: kvrocks'
// prepare_latest_account_state_row omits data_boc entirely once the account data
// exceeds max_data_depth, and a shard state never truncates.
#pragma once

#include "Value.h"

#include "td/utils/Status.h"
#include "vm/cells/Cell.h"

namespace mch {

// Step 1 — the stake wallet's data cell: two addresses, no header.
// `staking_pool` is a VType::Account Value (the shape the record field wants);
// an addr_none pool is an ERROR here, because Python's staking_pool.to_str()
// raises into the outer except and nulls the whole record.
// `minter` is nullopt when the slot is addr_none — that is NOT fatal, it only
// leaves jvault_asset null.
struct JvaultStakeWallet {
  Value staking_pool;                  // VType::Account, never addr_none
  std::string staking_pool_raw;        // canonical "wc:HEX" for the next lookup
  std::optional<std::string> minter;   // canonical "wc:HEX"
};
td::Result<JvaultStakeWallet> parse_jvault_stake_wallet(const td::Ref<vm::Cell> &data);

// Step 2 — the staking pool's data cell: skip 1+32 bits, then admin, creator,
// and THIRD the lock wallet (address_selectors.py derive_accounts_from_stake_pool).
// An error here is Python's KeyError on ['lock_wallet_address'], which nulls
// the whole record — NOT the partial case.
td::Result<std::string> parse_jvault_pool_lock_wallet(const td::Ref<vm::Cell> &data);

// The record the lookup yields: Obj{staking_pool, asset, jvault_asset}.
// `asset` null is the legitimate PARTIAL case (pool resolved, its state absent).
Value jvault_record(const Value &staking_pool, Value asset, Value jvault_asset);

// Asset(jetton=addr) from a canonical raw address, or Null for nullopt.
Value jvault_asset_of(const std::optional<std::string> &canonical_master);

}  // namespace mch
