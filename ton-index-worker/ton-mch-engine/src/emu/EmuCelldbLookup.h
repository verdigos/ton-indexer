// W3 — the synchronous celldb tier-2 lookup hook.
//
// Tier 1 resolves an address only if the trace itself touched that account.
// The class it structurally cannot serve is an address parsed out of a MESSAGE
// BODY that never got a transaction — the DeDust pools past a failed hop, the
// stonfi/tonco counterparty wallets, the jvault staking pool. Python has a whole
// prefetch module for exactly these (address_selectors.py extract_additional_
// addresses); here they are read straight out of the shard states the trace was
// emulated against.
//
// Why this needs no suspension/fixpoint/breaker machinery: a celldb read is
// local, in-process and SYNCHRONOUS, so it fits behind the existing
// ParsedBlockLookupSource::Tier2Hook seam and BuildDriver::run_two_phase already
// resolves lookup-arg-of-lookup within one build invocation. There is no socket,
// so there is nothing to break.
//
// The hook NEVER builds a Value of its own (§4d step 6): it produces a
// schema::BlockchainInterfaceV2 and renders it through
// ParsedBlockLookupSource::iface_value — the same call tier 1 makes. The two
// tiers are then shape-identical by construction rather than by review, which
// also means W1's nft_item.content fix is inherited here with no second copy.
//
// One instance per classify. It holds the memo and the budget, so its lifetime
// IS the scope both are measured over.
#pragma once

#include "ParsedBlockLookupSource.h"  // Value, schema::*, AllShardStates via IndexData.h

#include "smc-interfaces/FetchAccountFromShard.h"  // AllShardStates, lookup_account

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace mch {

// Replaces the archive's breaker + suspended-cap + alarm sweep. Both limits
// degrade to Value::null(), which is exactly the tier-1-only behaviour that
// shipped before this wave — a blown budget costs resolution, never correctness.
// The numbers are GUESSES: there is no in-tree measurement of a cold celldb
// point read plus a TVM run_get_method. Re-set them from the first live run.
struct Tier2Budget {
  std::size_t fetches{16};      // distinct celldb account reads per classify
  std::int64_t wall_us{20'000}; // wall-clock ceiling per classify
};

struct Tier2Stats {
  std::size_t fetched{0};           // celldb account reads issued
  std::size_t memo_hits{0};         // repeat (kind, address) served from the memo
  std::size_t budget_exhausted{0};  // account reads refused by a budget
};

class EmuCelldbTier2 {
 public:
  // A celldb account read. Injectable ONLY so the hermetic gate can substitute a
  // map for the shard states — the product default is always lookup_account.
  using AccountSource = std::function<td::Result<schema::AccountState>(const block::StdAddress &)>;

  // `shard_states` and `config` must outlive this object (they are the view's).
  EmuCelldbTier2(const AllShardStates *shard_states, std::shared_ptr<block::ConfigInfo> config,
                 Tier2Budget budget);

  // Tier2Hook shape. Never throws, never faults: every failure is a null, i.e.
  // the pre-W3 result.
  Value fetch(const std::string &kind, const std::vector<Value> &args);

  const Tier2Stats &stats() const { return stats_; }
  void set_account_source(AccountSource src) { account_source_ = std::move(src); }

 private:
  // Memoized account state; nullptr == absent, unreadable, or budget refused.
  // The memo is MANDATORY, not an optimisation: run_two_phase re-runs fetch
  // across rounds and every hop of a multi-pool swap asks for the same pools.
  const schema::AccountState *account(const block::StdAddress &addr);

  Value resolve(const std::string &kind, const block::StdAddress &addr);
  Value jvault_assets(const block::StdAddress &stake_wallet);

  const AllShardStates *shard_states_;
  std::shared_ptr<block::ConfigInfo> config_;
  Tier2Budget budget_;
  std::int64_t deadline_us_;
  AccountSource account_source_;

  std::unordered_map<block::StdAddress, std::optional<schema::AccountState>> account_memo_;
  std::unordered_map<std::string, Value> value_memo_;
  Tier2Stats stats_;
};

}  // namespace mch
