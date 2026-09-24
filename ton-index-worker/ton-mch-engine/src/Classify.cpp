#include "Classify.h"

#include <utility>

namespace mch {

namespace {

TieredLookupSource::StableAddressSet compute_status_stable_accounts(
    const std::vector<schema::Transaction> &txs) {
  TieredLookupSource::StableAddressSet stable;
  TieredLookupSource::StableAddressSet unstable;
  for (const auto &tx : txs) {
    if ((tx.orig_status != schema::active || tx.end_status == schema::active) &&
        unstable.find(tx.account) == unstable.end()) {
      stable.insert(tx.account);
    } else {
      stable.erase(tx.account);
      unstable.insert(tx.account);
    }
  }
  return stable;
}

}  // namespace

ClassifyRun classify(
    const MchEnginePrep &prep, const std::string &trace_id,
    const std::vector<schema::Transaction> &txs,
    const TieredLookupSource::InterfaceMap &interfaces,
    const AllShardStates *shard_states, std::shared_ptr<block::ConfigInfo> config) {
  TieredLookupSource::StableAddressSet status_stable_accounts =
      compute_status_stable_accounts(txs);
  CelldbTier2Lookup tier2(shard_states, std::move(config));
  TieredLookupSource::Tier2Hook hook;
  if (shard_states != nullptr) {
    hook = [&tier2](const std::string &kind, const std::vector<Value> &args) {
      return tier2.fetch(kind, args);
    };
  }
  TieredLookupSource src(&interfaces, std::move(hook), &status_stable_accounts);

  ClassifyRun result;
  result.classified = classify_schema_trace(prep, trace_id, txs, src);
  result.lookup_stats = src.stats();
  result.tier2_stats = tier2.stats();
  return result;
}

}  // namespace mch
