#include "ClassifyDriver.h"

#include "BuildRuntime.h"      // lookup_key
#include "SchemaTraceLoader.h"  // schema_to_trace

#include <utility>

namespace mch {

FixpointStep ClassifyFixpoint::step() {
  // A failure after round 0 would need `make_ctx_` to be non-deterministic — it
  // is not (same txs / same fixture bytes) — and the pass just run has already
  // been superseded, so there is nothing to commit either way.
  auto r_ctx = make_ctx_();
  if (r_ctx.is_error()) {
    result_.malformed = true;
    result_.error = r_ctx.move_as_error().message().str();
    committed_ = true;
    return FixpointStep::committed;
  }
  std::unique_ptr<TraceContext> ctx = r_ctx.move_as_ok();

  wanted_.clear();
  ParsedBlockLookupSource::Tier2Hook record =
      [this](const std::string &kind, const std::vector<Value> &args) {
        const std::string key = lookup_key(kind, args);
        auto it = fetched_.find(key);
        if (it != fetched_.end()) {
          return it->second;  // resolved, or a known null
        }
        wanted_.emplace(key, std::make_pair(kind, args));
        return Value::null();  // exactly the tier-2-unset behaviour
      };
  RoundSource rs = make_src_(*ctx, std::move(record));

  ClassifyResult res = classify_trace(*ctx, ir_, setup_, *rs.src);
  rounds_++;
  result_.rounds = rounds_;
  result_.requested.insert(wanted_.begin(), wanted_.end());
  if (rs.stats != nullptr) {
    result_.lookup_stats = *rs.stats;  // overwritten per round: the last one wins
  }
  pass_ctx_ = std::move(ctx);
  pass_res_ = std::move(res);

  const bool at_cap = rounds_ >= max_rounds_;
  if (wanted_.empty() || at_cap) {
    result_.rounds_exhausted = !wanted_.empty() && at_cap;
    commit();
    return FixpointStep::committed;
  }
  return FixpointStep::wants_lookups;
}

void ClassifyFixpoint::feed(const std::map<std::string, Value> &values) {
  for (const auto &kv : values) {
    fetched_.insert(kv);  // may include keys we did not ask for
  }
  // Unanswered => an explicit null, so the fixpoint cannot spin on a
  // permanently-absent key.
  for (const auto &[key, want] : wanted_) {
    fetched_.emplace(key, Value::null());
  }
  wanted_.clear();
}

void ClassifyFixpoint::commit_current() {
  if (!committed_) {
    commit();
  }
}

void ClassifyFixpoint::commit() {
  result_.ctx = std::move(pass_ctx_);
  result_.res = std::move(pass_res_);
  committed_ = true;
}

DriverResult run_classify_fixpoint(const ContextBuilder &make_ctx,
                                   const RoundSourceBuilder &make_src, const LoadedIr &ir,
                                   const ClassifySetup &setup, const Tier2Fetch &fetch,
                                   std::size_t max_rounds) {
  ClassifyFixpoint fx(make_ctx, make_src, ir, setup, max_rounds);
  while (fx.step() == FixpointStep::wants_lookups) {
    if (!fetch) {
      // No tier-2 at all: commit round 0, byte-identical to the pre-driver
      // single-pass code.
      fx.commit_current();
      break;
    }
    fx.feed(fetch(fx.wanted()));
  }
  return fx.take_result();
}

std::unique_ptr<ClassifyFixpoint> make_trace_fixpoint(
    const std::string &trace_id, const std::vector<schema::Transaction> &txs,
    const ParsedBlockLookupSource::InterfaceMap &ifaces, const LoadedIr &ir,
    const ClassifySetup &setup, std::size_t max_rounds) {
  auto make_ctx = [&trace_id, &txs]() -> td::Result<std::unique_ptr<TraceContext>> {
    TRY_RESULT(trace, schema_to_trace(trace_id, txs));
    auto ctx = std::make_unique<TraceContext>();
    ctx->trace = std::move(trace);
    ctx->tree = to_tree(ctx->trace);
    if (ctx->tree.root == nullptr) {
      return td::Status::Error("empty event tree");
    }
    ctx->root = init_block(ctx->arena, ctx->tree.root);
    return ctx;
  };
  auto make_src = [&ifaces](TraceContext &, ParsedBlockLookupSource::Tier2Hook record) {
    auto src = std::make_shared<ParsedBlockLookupSource>(&ifaces, std::move(record));
    const ParsedBlockLookupSource::LookupStats *stats = &src->stats();
    return RoundSource{std::move(src), stats};
  };
  return std::make_unique<ClassifyFixpoint>(std::move(make_ctx), std::move(make_src), ir, setup,
                                            max_rounds);
}

DriverResult classify_trace_fixpoint(const std::string &trace_id,
                                     const std::vector<schema::Transaction> &txs,
                                     const ParsedBlockLookupSource::InterfaceMap &ifaces,
                                     const LoadedIr &ir, const ClassifySetup &setup,
                                     const Tier2Fetch &fetch, std::size_t max_rounds) {
  // The SAME builders the suspending actor uses — one definition, so the
  // asynchronous path cannot drift from the synchronous one.
  auto fx = make_trace_fixpoint(trace_id, txs, ifaces, ir, setup, max_rounds);
  while (fx->step() == FixpointStep::wants_lookups) {
    if (!fetch) {
      fx->commit_current();
      break;
    }
    fx->feed(fetch(fx->wanted()));
  }
  return fx->take_result();
}

}  // namespace mch
