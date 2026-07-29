// kvrocks payload -> engine `Value` decoding (em2-tier2-design §6.1/§6.2, W3).
//
// PURE: JSON string in, Value out. No redis, no actors, no IO — the transport
// (one blocking MGET per table on a lookup-actor pool, §5.3/§6.5) lives on the
// mch-emu side and hands the payload bytes here. That split is what lets the
// whole mapping be gated offline against payload vectors.
//
// The target shapes are ParsedBlockLookupSource's tier-1 shapes — the A/B parity
// contract: a lookup must yield the same Obj whether it was served from the
// in-block interfaces or from the store. For the two tier-2-only kinds
// (nominator_pool, jvault_assets) the target is FixtureLookupSource's shape.
//
// Encodings, read off the WRITER (`KvrocksState.cpp`'s json_put_* helpers), not
// off the design doc: addresses are raw "wc:HEX" strings (`address_key`), int256
// arrive as DECIMAL STRINGS (`to_dec_string`), hashes as base64, bools as JSON
// bools, u64 as decimal strings, and an absent optional is an explicit JSON null.
//
// `destroyed` is PER TABLE. Python treats a `"destroyed": true` payload as
// absent (`interface_repository.py:126-127`); the field exists on jetton_wallets,
// nft_items, getgems_nft_sales, getgems_nft_auctions and dex_pools, but NOT on
// latest_account_states — whose payload builder emits no such field — so the rule
// is a no-op for nominator_pool and for both jvault data_boc reads, exactly as in
// Python.
#pragma once

#include "BuildRuntime.h"    // Value
#include "ClassifyDriver.h"  // LookupWants (the collect pass's batch)

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mch {

// "ton-index:v1:<table>:<id>:payload" — mirrors KvrocksState.cpp:339-344.
std::string kvrocks_payload_key(const std::string &table, const std::string &id);

// The table a lookup kind reads (§6.1). jvault_assets has none of its own: it is
// a CHAIN over latest_account_states + jetton_wallets (§6.2), so it returns
// nullopt and the caller drives kvrocks_jvault_*.
std::optional<std::string> kvrocks_table_for_kind(const std::string &kind);

// Raw lookup argument -> the store id. Tier 1 is case-insensitive by accident
// (it parses via StdAddress), tier 2 is not: the writer's ids are UPPERCASE hex
// (`to_hex`), so a lowercase argument would be a silent 100 % miss. nullopt for
// a malformed address == tier 1's clean miss.
std::optional<std::string> kvrocks_address_id(const std::string &raw_address);

// Decode one payload for a non-chained kind. An absent row (empty payload), a
// destroyed row, malformed JSON or a missing required field all yield null —
// infrastructure problems degrade, never fault (§8.1).
Value decode_kvrocks_payload(const std::string &kind, const std::string &payload);

// --- jvault_assets, the 3-step chain (§6.2) ---------------------------------
//
// Step 1 parses the stake wallet's account-state data cell (staking_pool +
// minter). Step 2 parses the POOL's data cell for lock_wallet_address — the ~10
// lines of `derive_accounts_from_stake_pool` (address_selectors.py:128-139) the
// fixture source gets pre-derived. Step 3 reads that wallet's `jetton`.
//
// Partial results are SEMANTIC, not errors (jvault.py:58-59): a resolved pool
// whose own state is absent yields {staking_pool, asset=null, jvault_asset};
// anything failing later yields a null record.
struct JvaultStake {
  Value staking_pool;   // Account
  Value jvault_asset;   // Asset(jetton) or null
};

// nullopt == the whole record is null (Python's (None,None,None) sentinel).
std::optional<JvaultStake> kvrocks_jvault_stake(const std::string &state_payload);

// The pool's lock_wallet_address, as a store id. nullopt == null record.
std::optional<std::string> kvrocks_jvault_lock_wallet(const std::string &pool_state_payload);

// Assemble the record. `asset` null (pool state absent) is the partial case.
Value kvrocks_jvault_record(const JvaultStake &stake, Value asset);

// The `jetton` of a jetton_wallets payload, as an Asset — step 3's tail.
// Null when the wallet is absent/destroyed or carries no jetton.
Value kvrocks_jvault_asset_from_wallet(const std::string &wallet_payload);

// --- batch resolution (§6.5) -------------------------------------------------

// Fetch payloads for a set of keys in ONE round trip. An absent key must come
// back as an EMPTY string, not be omitted — the resolver turns misses into
// explicit nulls, which is what keeps the trace-level fixpoint from re-asking
// for a permanently-absent key every round. The transport (a blocking pipelined
// MGET per table on a lookup actor, §5.3) supplies this; keeping it injectable
// is what makes the mapping gateable with no store.
using PayloadFetch =
    std::function<std::map<std::string, std::string>(const std::vector<std::string> &keys)>;

// Resolve one collect-pass batch: group by table, one fetch for everything that
// can be answered directly (nominator_pool and jvault's first step share the
// latest_account_states fetch), then AT MOST two more fetches for the jvault
// chain — 3 chained round trips worst case, 1 in the common case.
// Every requested key appears in the result; a miss is an explicit null.
std::map<std::string, Value> kvrocks_resolve_batch(const LookupWants &wanted,
                                                   const PayloadFetch &fetch);

}  // namespace mch
