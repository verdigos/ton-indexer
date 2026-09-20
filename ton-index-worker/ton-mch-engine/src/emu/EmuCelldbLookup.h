// Synchronous shard-state fallback for addresses absent from tier 1. One
// instance owns the memo for one classification.
#pragma once

#include "ParsedBlockLookupSource.h"  // Value, schema::*, AllShardStates via IndexData.h

#include "smc-interfaces/FetchAccountFromShard.h"  // AllShardStates, lookup_account

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace mch {

struct Tier2Stats {
  std::size_t fetched{0};    // celldb account reads issued
  std::size_t memo_hits{0};  // repeat (kind, address) served from the memo
  double account_load_seconds{0};
  double data_load_seconds{0};
  double detect_seconds{0};
};

class EmuCelldbTier2 {
 public:
  // Injectable account reader; production uses lookup_account.
  using AccountSource = std::function<td::Result<std::optional<schema::AccountState>>(const block::StdAddress &)>;

  // `shard_states` and `config` must outlive this object (they are the view's).
  EmuCelldbTier2(const AllShardStates *shard_states, std::shared_ptr<block::ConfigInfo> config);

  // Detector misses are null; account read errors also flag unavailable history.
  ParsedBlockLookupSource::Tier2Result fetch(const std::string &kind, const std::vector<Value> &args);

  const Tier2Stats &stats() const { return stats_; }
  bool history_unavailable() const { return history_unavailable_; }
  void set_account_source(AccountSource src) { account_source_ = std::move(src); }

 private:
  // Memoized account state. nullptr covers absence or read failure.
  // Fixpoint rounds reuse the memo and must not repeat account reads.
  const schema::AccountState *account(const block::StdAddress &addr);

  Value resolve(const std::string &kind, const block::StdAddress &addr);
  Value jvault_assets(const block::StdAddress &stake_wallet);
  Value telemint_collection(const block::StdAddress &addr);

  const AllShardStates *shard_states_;
  std::shared_ptr<block::ConfigInfo> config_;
  AccountSource account_source_;

  std::unordered_map<block::StdAddress, std::optional<schema::AccountState>> account_memo_;
  std::unordered_map<std::string, Value> value_memo_;
  Tier2Stats stats_;
  std::size_t fetch_depth_{0};
  bool history_unavailable_{false};
};

}  // namespace mch
