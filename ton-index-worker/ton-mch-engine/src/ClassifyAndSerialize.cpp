#include "ClassifyAndSerialize.h"

#include <utility>

namespace mch {

namespace {

TieredLookupSource::StableAddressSet status_stable_accounts(
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

bool is_failure(TraceOutcome outcome) {
  return outcome == TraceOutcome::fallback || outcome == TraceOutcome::terminal_failure_empty;
}

}  // namespace

ClassifyAndSerializeResult classify_and_serialize(
    const MchEnginePrep &prep, const std::vector<schema::Transaction> &txs,
    const EmuTraceView &view, const TieredLookupSource::InterfaceMap &interfaces,
    ClassifyAndSerializeOptions options) {
  auto stable = status_stable_accounts(txs);
  TieredLookupSource::StableAddressSet no_authoritative_accounts;
  const auto *authoritative_accounts =
      options.authoritative_tier1 ? &stable : &no_authoritative_accounts;

  CelldbTier2Lookup tier2(&view.shard_states, view.config);
  TieredLookupSource::Tier2Hook hook;
  if (options.tier2_enabled) {
    hook = std::move(options.tier2_hook);
    if (!hook) {
      hook = [&tier2](const std::string &kind, const std::vector<Value> &args) {
        return tier2.fetch(kind, args);
      };
    }
  }
  TieredLookupSource src(&interfaces, std::move(hook), authoritative_accounts);

  ClassifyAndSerializeResult result;
  result.classified = classify_schema_trace(prep, view.trace_id, txs, src);
  result.lookup_stats = src.stats();
  result.tier2_stats = tier2.stats();

  if (options.serialize_failure_rows || !is_failure(result.classified.outcome)) {
    const auto started_us = emu_now_us();
    result.actions_blob =
        serialize_redis_actions(result.classified.rows, view, &result.serialize_stats);
    result.serialize_us = emu_now_us() - started_us;
  }
  return result;
}

}  // namespace mch
