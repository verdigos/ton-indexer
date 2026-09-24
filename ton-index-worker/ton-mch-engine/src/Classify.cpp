#include "Classify.h"

#include <algorithm>
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

bool merge_trace_interface_memo(TieredLookupSource::InterfaceMap &memo,
                                const schema::Transaction &tx,
                                const TieredLookupSource::InterfaceMap &block_interfaces) {
  auto detected = block_interfaces.find(tx.account);
  if (detected == block_interfaces.end()) {
    return false;
  }
  auto [slot, inserted] = memo.try_emplace(tx.account);
  auto &records = slot->second;
  for (const auto &iface : detected->second) {
    auto existing = std::find_if(records.begin(), records.end(), [&](const auto &record) {
      return record.index() == iface.index();
    });
    if (existing == records.end()) {
      records.push_back(iface);
    } else {
      *existing = iface;
    }
  }
  return inserted;
}

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
