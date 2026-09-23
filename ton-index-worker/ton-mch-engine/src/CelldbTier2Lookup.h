// Synchronous shard-state fallback for addresses absent from tier 1. One
// instance owns the memo for one classification.
#pragma once

#include "TieredLookupSource.h"  // Value, schema::*, AllShardStates via IndexData.h

#include "smc-interfaces/FetchAccountFromShard.h"  // AllShardStates, lookup_account
#include "smc-interfaces/NftSaleV4.h"
#include "smc-interfaces/Telemint.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace mch {

struct Tier2Stats {
  std::size_t fetched{0};    // celldb account reads issued
  std::size_t memo_hits{0};  // repeat (kind, address) served from the memo
};

class CelldbTier2Lookup {
 public:
  // Injectable account reader; production uses lookup_account.
  using AccountSource = std::function<td::Result<std::optional<schema::AccountState>>(const block::StdAddress &)>;
  using NftSaleV4Detector = std::function<td::Result<GetGemsNftFixPriceSaleV4::Result>(
      const block::StdAddress &, const td::Ref<vm::Cell> &, const td::Ref<vm::Cell> &,
      const std::shared_ptr<block::ConfigInfo> &)>;
  using TelemintDetector = std::function<td::Result<TelemintContract::Result>(
      const block::StdAddress &, const td::Ref<vm::Cell> &, const td::Ref<vm::Cell> &,
      const std::shared_ptr<block::ConfigInfo> &)>;

  // `shard_states` and `config` must outlive this object (they are the view's).
  CelldbTier2Lookup(const AllShardStates *shard_states, std::shared_ptr<block::ConfigInfo> config,
                    TelemintDetector telemint_detector = {});

  // Detector misses and account read errors are null.
  Value fetch(const std::string &kind, const std::vector<Value> &args);

  const Tier2Stats &stats() const { return stats_; }
  void set_account_source(AccountSource src) { account_source_ = std::move(src); }
  void set_nft_sale_v4_detector(NftSaleV4Detector detector) { nft_sale_v4_detector_ = std::move(detector); }

 private:
  // Memoized account state. nullptr covers absence or read failure.
  // Fixpoint rounds reuse the memo and must not repeat account reads.
  const schema::AccountState *account(const block::StdAddress &addr);

  Value resolve(const std::string &kind, const block::StdAddress &addr);
  Value jvault_assets(const block::StdAddress &stake_wallet);

  const AllShardStates *shard_states_;
  std::shared_ptr<block::ConfigInfo> config_;
  AccountSource account_source_;
  NftSaleV4Detector nft_sale_v4_detector_;
  TelemintDetector telemint_detector_;

  std::unordered_map<block::StdAddress, std::optional<schema::AccountState>> account_memo_;
  std::unordered_map<std::string, Value> value_memo_;
  Tier2Stats stats_;
};

}  // namespace mch
