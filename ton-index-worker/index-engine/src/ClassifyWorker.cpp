#include "ClassifyWorker.h"

#include "Statistics.h"
#include "emu/EmuCelldbLookup.h"
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
    ParsedBlockLookupSource::StableAddressSet status_stable_accounts;
    ParsedBlockLookupSource::StableAddressSet status_unstable_accounts;
    for (const auto &tx : job.input.txs) {
      if ((tx.orig_status != schema::active || tx.end_status == schema::active) &&
          status_unstable_accounts.find(tx.account) == status_unstable_accounts.end()) {
        status_stable_accounts.insert(tx.account);
      } else {
        status_stable_accounts.erase(tx.account);
        status_unstable_accounts.insert(tx.account);
      }
    }
    EmuCelldbTier2 tier2(&states, job.block->mc_block_.config_);
    double lookup_seconds = 0;
    ParsedBlockLookupSource src(&job.input.interfaces,
        [&tier2, &lookup_seconds](const std::string &kind, const std::vector<Value> &args,
                                 LookupReason reason) {
          td::Timer lookup_timer;
          auto result = tier2.fetch(kind, args);
          const double elapsed_seconds = lookup_timer.elapsed();
          lookup_seconds += elapsed_seconds;
          const auto *matcher = reject_ctx().matcher;
          record_lookup_stat(matcher ? *matcher : "<none>", kind,
                             static_cast<std::uint64_t>(elapsed_seconds * 1e6),
                             result.value.is_null(), reason);
          return result;
        }, &status_stable_accounts);
    const auto key = td::base64_encode(job.input.trace.trace_id.as_slice());
    auto r = classify_schema_trace(*prep_, key, job.input.txs, src);
    TraceClassificationResult result{std::move(job.input.trace), r.outcome, std::move(r.rows), std::move(r.failure_reason), false, std::move(r.rescue)};
    const double seconds = timer.elapsed();
    g_statistics.record_time(MCH_CLASSIFY_TRACE, seconds * 1e6);
    if (r.prepare_micros || r.match_micros || r.build_micros) {
      g_statistics.record_time(MCH_CLASSIFY_PREPARE, r.prepare_micros);
      // Match includes celldb lookup time by design.
      g_statistics.record_time(MCH_CLASSIFY_MATCH, r.match_micros);
      g_statistics.record_time(MCH_CLASSIFY_BUILD, r.build_micros);
      g_statistics.record_time(MCH_CLASSIFY_LOOKUP, lookup_seconds * 1e6);
    }
    g_statistics.record_time(MCH_LOOKUP_ACCOUNT_LOAD,
                             tier2.stats().account_load_seconds * 1e6);
    g_statistics.record_time(MCH_LOOKUP_DATA_LOAD,
                             tier2.stats().data_load_seconds * 1e6);
    g_statistics.record_time(MCH_LOOKUP_DETECT,
                             tier2.stats().detect_seconds * 1e6);
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
