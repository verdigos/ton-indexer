#include "MchClassifierActor.h"

#include "BlockTree.h"
#include "ParsedBlockLookupSource.h"
#include "SchemaTraceLoader.h"

#include "td/utils/base64.h"
#include "td/utils/logging.h"

#include <exception>
#include <map>
#include <set>
#include <unordered_map>

namespace mch {

td::Result<std::shared_ptr<const MchEnginePrep>> load_engine_prep(
    const std::string &artifact_path) {
  TRY_RESULT(ir, load_ir(artifact_path));
  auto prep = std::make_shared<MchEnginePrep>();
  prep->ir = std::move(ir);
  prep->setup = prepare_classify(prep->ir);
  if (prep->setup.table_missing || prep->setup.fn_missing) {
    // prod-design §4: an unrunnable artifact matcher is a build bug — surface it
    // as an error so the caller can be startup-fatal in production.
    return td::Status::Error(prep->setup.error);
  }
  return std::shared_ptr<const MchEnginePrep>(std::move(prep));
}

namespace {

// One assembled trace -> a classify summary (never throws). Builds the engine
// TraceContext from the schema transactions (SchemaTraceLoader), then classifies
// with a tier-1-only ParsedBlockLookupSource (tier-2 DB/kvrocks fallback is a
// later slice — the per-seqno account_interfaces_ map is all we read here).
TraceClassifySummary classify_one(const MchEnginePrep &prep,
                                  const ParsedBlockLookupSource::InterfaceMap &ifaces,
                                  const std::string &trace_id,
                                  const std::vector<schema::Transaction> &txs) {
  TraceClassifySummary sum;
  sum.trace_id = trace_id;
  try {
    auto r_trace = schema_to_trace(trace_id, txs);
    if (r_trace.is_error()) {
      sum.failure = true;
      sum.failure_reason = r_trace.move_as_error().message().str();
      sum.failure_category = FailureCategory::malformed_trace;
      return sum;
    }
    TraceContext ctx;
    ctx.trace = r_trace.move_as_ok();
    ctx.tree = to_tree(ctx.trace);
    if (ctx.tree.root == nullptr) {
      sum.failure = true;
      sum.failure_reason = "empty event tree";
      sum.failure_category = FailureCategory::malformed_trace;
      return sum;
    }
    ctx.root = init_block(ctx.arena, ctx.tree.root);

    ParsedBlockLookupSource src(&ifaces);  // tier-2 hook unset: in-block only
    ClassifyResult res = classify_trace(ctx, prep.ir, prep.setup, src);
    sum.failure = res.failure;
    sum.failure_reason = res.failure_reason;
    sum.failure_category = res.failure_category;
    sum.lookup_stats = src.stats();
    sum.action_count = res.actions.size();
    std::set<std::string> names;
    for (const CoreAction &a : res.actions) {
      names.insert(a.matcher_name);
    }
    sum.matcher_names.assign(names.begin(), names.end());
  } catch (const std::exception &e) {
    sum.failure = true;
    sum.failure_reason = std::string("exception: ") + e.what();
    sum.failure_category = FailureCategory::engine_fault;
  } catch (...) {
    sum.failure = true;
    sum.failure_reason = "unknown exception";
    sum.failure_category = FailureCategory::engine_fault;
  }
  return sum;
}

const char *category_name(FailureCategory c) {
  switch (c) {
    case FailureCategory::none: return "none";
    case FailureCategory::matcher_skip: return "matcher_skip";
    case FailureCategory::clean_reject: return "clean_reject";
    case FailureCategory::parse_null: return "parse_null";
    case FailureCategory::lookup_miss_tier1: return "lookup_miss_tier1";
    case FailureCategory::lookup_miss_tier2: return "lookup_miss_tier2";
    case FailureCategory::lookup_infra_fail: return "lookup_infra_fail";
    case FailureCategory::engine_fault: return "engine_fault";
    case FailureCategory::malformed_trace: return "malformed_trace";
  }
  return "?";
}

}  // namespace

void MchClassifierActor::start_up() {
  // Group the block's transactions by trace (schema::Transaction.trace_id — the
  // field TraceAssembler assigns; schema::Trace itself carries only edges, no tx
  // list). Transactions live under ParsedBlock.blocks_[*].transactions.
  std::unordered_map<td::Bits256, std::vector<schema::Transaction>> by_trace;
  if (block_ != nullptr) {
    for (const auto &blk : block_->blocks_) {
      for (const auto &tx : blk.transactions) {
        by_trace[tx.trace_id].push_back(tx);
      }
    }
  }

  const ParsedBlockLookupSource::InterfaceMap empty_ifaces;
  const ParsedBlockLookupSource::InterfaceMap &ifaces =
      block_ != nullptr ? block_->account_interfaces_ : empty_ifaces;

  std::size_t traces = 0, total_actions = 0, failures = 0;
  std::map<FailureCategory, std::size_t> category_counts;  // per-category trace failures
  ParsedBlockLookupSource::LookupStats agg_lookups;        // summed tier attribution
  for (const auto &[trace_id, txs] : by_trace) {
    std::string tid = td::base64_encode(trace_id.as_slice());
    TraceClassifySummary sum = classify_one(*prep_, ifaces, tid, txs);
    traces++;
    total_actions += sum.action_count;
    agg_lookups.tier1_hits += sum.lookup_stats.tier1_hits;
    agg_lookups.tier2_hits += sum.lookup_stats.tier2_hits;
    agg_lookups.misses += sum.lookup_stats.misses;
    if (sum.failure) {
      failures++;
      category_counts[sum.failure_category]++;
      LOG(WARNING) << "[mch-shadow] seqno=" << mc_seqno_ << " trace=" << tid
                   << " FAILED (" << category_name(sum.failure_category)
                   << "): " << sum.failure_reason;
    } else {
      std::string names;
      for (const std::string &n : sum.matcher_names) {
        if (!names.empty()) names += ",";
        names += n;
      }
      LOG(DEBUG) << "[mch-shadow] seqno=" << mc_seqno_ << " trace=" << tid
                 << " actions=" << sum.action_count << " matchers=[" << names << "]";
    }
  }
  // Per-category failure breakdown + tier-2 hit-rate (prod-design §3 metric).
  std::string cats;
  for (const auto &[cat, n] : category_counts) {
    if (!cats.empty()) cats += ",";
    cats += std::string(category_name(cat)) + "=" + std::to_string(n);
  }
  std::size_t resolved = agg_lookups.tier1_hits + agg_lookups.tier2_hits;
  LOG(INFO) << "[mch-shadow] seqno=" << mc_seqno_ << " traces=" << traces
            << " actions=" << total_actions << " failures=" << failures << " [" << cats << "]"
            << " lookups{tier1=" << agg_lookups.tier1_hits << " tier2=" << agg_lookups.tier2_hits
            << " miss=" << agg_lookups.misses << " tier2_rate="
            << (resolved ? (100 * agg_lookups.tier2_hits / resolved) : 0) << "%}";

  stop();
}

}  // namespace mch
