#include "ClassifyWorker.h"

#include "CelldbTier2Lookup.h"
#include "td/utils/Timer.h"
#include "td/utils/base64.h"

#include <exception>

namespace mch {

ClassifyWorker::ClassifyWorker(std::shared_ptr<const MchEnginePrep> prep, ClassifyPolicy policy)
    : prep_(std::move(prep)), policy_(policy) {}

void ClassifyWorker::classify(ClassifyJob job, td::Promise<TraceClassificationResult> promise) {
  try {
    td::Timer timer;
    if (!prep_ || !job.block) {
      promise.set_error(td::Status::Error("Classification requires engine prep and a completion block"));
      return;
    }
    AllShardStates states;
    for (const auto &shard : job.block->mc_block_.shard_blocks_) {
      states.push_back(shard.block_state);
    }
    TieredLookupSource::StableAddressSet status_stable_accounts;
    TieredLookupSource::StableAddressSet status_unstable_accounts;
    for (const auto &tx : job.input.txs) {
      if ((tx.orig_status != schema::active || tx.end_status == schema::active) &&
          status_unstable_accounts.find(tx.account) == status_unstable_accounts.end()) {
        status_stable_accounts.insert(tx.account);
      } else {
        status_stable_accounts.erase(tx.account);
        status_unstable_accounts.insert(tx.account);
      }
    }
    CelldbTier2Lookup tier2(&states, job.block->mc_block_.config_);
    TieredLookupSource src(&job.input.interfaces,
        [&tier2](const std::string &kind, const std::vector<Value> &args) {
          return tier2.fetch(kind, args);
        }, &status_stable_accounts);
    const auto key = td::base64_encode(job.input.trace.trace_id.as_slice());
    auto r = classify_schema_trace(*prep_, key, job.input.txs, src);
    TraceClassificationResult result{std::move(job.input.trace), r.outcome, std::move(r.rows), std::move(r.failure_reason), false, std::move(r.rescue)};
    result.tier1_hits = src.stats().tier1_hits;
    result.tier2_hits = src.stats().tier2_hits;
    result.lookup_misses = src.stats().misses;
    result.lookup_misses_by_kind = src.stats().misses_by_kind;
    result.tier2_fetched = tier2.stats().fetched;
    result.tier2_memo_hits = tier2.stats().memo_hits;
    const double seconds = timer.elapsed();
    result.slow = seconds > policy_.slow_job_threshold_s;
    if (result.slow) {
      LOG(WARNING) << "Slow classification trace=" << key << " seconds=" << seconds;
    }
    promise.set_value(std::move(result));
  } catch (const std::exception &e) {
    promise.set_error(td::Status::Error(td::Slice(e.what())));
  } catch (...) {
    promise.set_error(td::Status::Error("Unknown classification worker exception"));
  }
}

}  // namespace mch
