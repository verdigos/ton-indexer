#include "ReplayRun.h"

#include <algorithm>

namespace mch {

bool snapshot_usable(const SnapshotInfo &s, std::uint32_t next_seqno, std::uint32_t oldest_available) {
  const auto floor = std::max(oldest_available, std::uint32_t{1});
  return std::uint64_t{s.seqno} + 1 >= floor && s.seqno < next_seqno;
}

ReplayRun live_bounds(std::uint32_t next_seqno, std::uint32_t pg_next_seqno,
                      std::uint32_t oldest_available, std::optional<SnapshotInfo> snapshot) {
  ReplayRun run;
  const auto floor = std::max(oldest_available, std::uint32_t{1});
  run.no_insert_below = std::max(next_seqno, std::uint32_t{1});
  run.classify_from = std::max(pg_next_seqno, std::uint32_t{1});
  run.scan_from = snapshot && snapshot_usable(*snapshot, run.no_insert_below, floor)
      ? std::min(snapshot->oldest_healthy, snapshot->seqno + 1)
      : run.no_insert_below;
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

  std::int64_t earliest = in.from;
  if (!in.inventory.empty()) {
    earliest = UINT32_MAX;
    for (const auto &t : in.inventory) earliest = std::min<std::int64_t>(earliest, t.mc_seqno_start);
  }
  run.scan_from = std::max<std::int64_t>(std::max(in.oldest_available, std::uint32_t{1}), earliest);

  if (in.from < in.partitioned.first || in.to > in.partitioned.second || in.from > in.to) {
    plan.refusal = TaskPlanRefusal::no_partition;
  } else if (std::int64_t{in.to} > std::int64_t{in.live_frontier} - policy.live_margin) {
    plan.refusal = TaskPlanRefusal::inside_live_window;
  } else if (run.scan_from > earliest) {
    plan.refusal = TaskPlanRefusal::history_unavailable;
  }
  return plan;
}

}  // namespace mch
