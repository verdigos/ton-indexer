#include "ClassifyWorker.h"

#include "Classify.h"
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
    const auto key = td::base64_encode(job.input.trace.trace_id.as_slice());
    auto run = mch::classify(*prep_, key, job.input.txs, job.input.interfaces,
                             &states, job.block->mc_block_.config_);
    auto &r = run.classified;
    TraceClassificationResult result{std::move(job.input.trace), r.outcome, std::move(r.rows), std::move(r.failure_reason), false, std::move(r.rescue)};
    result.lookups = std::move(run.lookup_stats);
    result.tier2 = run.tier2_stats;
    result.skipped_payload_actions = r.skipped_payload_actions;
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
