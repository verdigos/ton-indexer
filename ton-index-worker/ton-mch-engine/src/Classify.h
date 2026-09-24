#pragma once

#include "CelldbTier2Lookup.h"
#include "ClassifyCore.h"

#include <memory>
#include <string>
#include <vector>

namespace mch {

struct ClassifyRun {
  SchemaClassifyResult classified;
  TieredLookupSource::LookupStats lookup_stats;
  Tier2Stats tier2_stats;
};

// Merge this transaction account's detected interfaces into one trace's memo.
// Returns true when the memo gains an account.
bool merge_trace_interface_memo(TieredLookupSource::InterfaceMap &memo,
                                const schema::Transaction &tx,
                                const TieredLookupSource::InterfaceMap &block_interfaces);

// A null shard-state pointer disables tier-2 lookups.
ClassifyRun classify(
    const MchEnginePrep &prep, const std::string &trace_id,
    const std::vector<schema::Transaction> &txs,
    const TieredLookupSource::InterfaceMap &interfaces,
    const AllShardStates *shard_states, std::shared_ptr<block::ConfigInfo> config);

}  // namespace mch
