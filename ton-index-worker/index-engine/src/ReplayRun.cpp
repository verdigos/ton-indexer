#include "ReplayRun.h"

#include <algorithm>

namespace mch {

ReplayRun live_bounds(std::uint32_t pg_next_seqno, std::optional<std::uint32_t> kvrocks_next_seqno,
                      std::uint32_t oldest_available, std::uint32_t lifetime_w) {
  ReplayRun run;
  run.policy.lifetime_w = lifetime_w;
  run.no_insert_below = std::min(pg_next_seqno, kvrocks_next_seqno.value_or(pg_next_seqno));
  run.classify_from = pg_next_seqno;
  run.scan_from = std::max<std::int64_t>(std::max(oldest_available, std::uint32_t{1}),
      std::min<std::int64_t>(std::int64_t{pg_next_seqno} - lifetime_w, run.no_insert_below));
  return run;
}

TaskPlan plan_task_run(const TaskPlanInput &in, const ClassifyPolicy &policy) {
  TaskPlan plan{ReplayRun{}, TaskPlanRefusal::none};
  auto &run = plan.run;
  run.mode = RunMode::task;
  run.policy = policy;
  run.scan_to = in.to;
  run.classify_from = in.from;
  run.classify_to = in.to;
  run.no_insert_below = UINT32_MAX;
  run.expected = in.inventory;
  run.classify_filter = in.trace_filter;

  std::int64_t earliest = std::int64_t{in.from} - policy.lifetime_w;
  std::uint64_t max_span = 0;
  if (!in.inventory.empty()) {
    earliest = UINT32_MAX;
    for (const auto &trace : in.inventory) {
      earliest = std::min<std::int64_t>(earliest, trace.mc_seqno_start);
      max_span = std::max<std::uint64_t>(max_span,
          std::max<std::int64_t>(0, std::int64_t{trace.mc_seqno_end} - trace.mc_seqno_start));
    }
    run.policy.lifetime_w = std::max<std::uint64_t>(policy.lifetime_w,
        std::min<std::uint64_t>(UINT32_MAX, max_span + policy.span_margin));
  }
  run.scan_from = std::max<std::int64_t>(std::max(in.oldest_available, std::uint32_t{1}), earliest);

  if (in.from < in.partitioned.first || in.to > in.partitioned.second || in.from > in.to) {
    plan.refusal = TaskPlanRefusal::no_partition;
  } else if (std::int64_t{in.to} > std::int64_t{in.live_frontier} - policy.live_margin) {
    plan.refusal = TaskPlanRefusal::inside_live_window;
  } else if (!in.inventory.empty() && run.scan_from > earliest) {
    plan.refusal = TaskPlanRefusal::history_unavailable;
  }
  return plan;
}

}  // namespace mch
