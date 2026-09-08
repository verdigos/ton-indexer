#include "TaskPlanning.h"

#include <algorithm>
#include <map>
#include <tuple>

namespace mch {

std::vector<TaskRun> group_consecutive(std::vector<ClaimedTask> tasks) {
  std::sort(tasks.begin(), tasks.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
  tasks.erase(std::unique(tasks.begin(), tasks.end(), [](const auto &a, const auto &b) {
    return a.id == b.id;
  }), tasks.end());
  std::sort(tasks.begin(), tasks.end(), [](const auto &a, const auto &b) {
    if (a.mc_seqno.has_value() != b.mc_seqno.has_value()) return a.mc_seqno.has_value();
    return std::tie(a.mc_seqno, a.id) < std::tie(b.mc_seqno, b.id);
  });

  std::vector<TaskRun> runs;
  for (const auto &task : tasks) {
    if (!task.mc_seqno) continue;
    const auto k = *task.mc_seqno;
    if (runs.empty() || std::uint64_t{k} > std::uint64_t{runs.back().to} + 1) {
      runs.push_back({k, k, std::nullopt, {}});
    }
    runs.back().to = k;
    runs.back().tasks.push_back(task);
  }
  for (const auto &task : tasks) {
    if (task.mc_seqno || !task.trace_id) continue;
    auto run = std::find_if(runs.begin(), runs.end(), [&](const auto &r) {
      return (!r.trace_filter && task.end >= r.from && task.end <= r.to) || r.trace_filter == task.trace_id;
    });
    if (run == runs.end()) {
      runs.push_back({task.start, task.end, task.trace_id, {task}});
    } else {
      run->tasks.push_back(task);
    }
  }
  return runs;
}

void validate_against_inventory(BlockClassification &bc, const std::vector<TraceInventory> &inventory) {
  std::map<td::Bits256, const TraceInventory *> expected;
  std::map<td::Bits256, std::size_t> counts;
  for (const auto &row : inventory) expected.emplace(row.trace_id, &row);
  for (const auto &trace : bc.traces) ++counts[trace.trace.trace_id];
  for (auto &trace : bc.traces) {
    const auto it = expected.find(trace.trace.trace_id);
    if (it == expected.end()) {
      trace.outcome = TraceOutcome::skipped_unexpected_trace;
    } else {
      const auto &row = *it->second;
      if (counts[row.trace_id] != 1 || trace.trace.nodes_ != row.nodes ||
          trace.trace.mc_seqno_start != row.mc_seqno_start || trace.trace.mc_seqno_end != row.mc_seqno_end) {
        trace.outcome = TraceOutcome::skipped_incomplete;
      }
    }
  }
  for (const auto &row : inventory) {
    if (counts[row.trace_id] != 0) continue;
    schema::Trace trace{};
    trace.trace_id = row.trace_id;
    trace.mc_seqno_start = row.mc_seqno_start;
    trace.mc_seqno_end = row.mc_seqno_end;
    trace.nodes_ = row.nodes;
    bc.traces.push_back({std::move(trace), TraceOutcome::skipped_incomplete, {}, {}});
  }
}

}  // namespace mch
