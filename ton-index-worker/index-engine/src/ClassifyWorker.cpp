#include "ClassifyWorker.h"

#include "emu/EmuCelldbLookup.h"
#include "td/utils/Timer.h"
#include "td/utils/base64.h"

#include <exception>

namespace mch {

ClassifyWorker::ClassifyWorker(std::shared_ptr<const MchEnginePrep> prep, ClassifyPolicy policy)
    : prep_(std::move(prep)), policy_(policy) {}

void ClassifyWorker::classify(ClassifyJob job, td::Promise<ClassifiedTrace> promise) {
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
    EmuCelldbTier2 tier2(&states, job.block->mc_block_.config_);
    ParsedBlockLookupSource src(&job.input.interfaces,
        [&tier2](const std::string &kind, const std::vector<Value> &args) {
          return tier2.fetch(kind, args);
        });
    const auto key = td::base64_encode(job.input.trace.trace_id.as_slice());
    auto r = classify_schema_trace(*prep_, key, job.input.txs, src);
    ClassifiedTrace result{std::move(job.input.trace), r.outcome, std::move(r.rows), std::move(r.failure_reason)};
    const double seconds = timer.elapsed();
    result.slow = seconds > policy_.job_timeout_s;
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
