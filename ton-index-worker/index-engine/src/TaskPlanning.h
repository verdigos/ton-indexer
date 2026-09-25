#pragma once

#include "ClassifyCore.h"
#include "ReplayRun.h"

namespace mch {

struct ClaimedTask {
  std::int32_t id;
  std::optional<std::uint32_t> mc_seqno;
  std::optional<td::Bits256> trace_id;
  std::uint32_t start{0}, end{0};
};

struct TaskRun {
  std::uint32_t from, to;
  std::optional<td::Bits256> trace_filter;
  std::vector<ClaimedTask> tasks;
};

// Deduplicates tasks by id, groups consecutive seqno tasks, and attaches each
// trace task to a run with the same trace id or a range containing its end.
// Tasks with neither key are dropped.
std::vector<TaskRun> group_consecutive(std::vector<ClaimedTask> tasks);

// Marks unexpected traces as skipped_unexpected_trace and duplicate or mismatched
// inventory traces as skipped_incomplete. Broken unexpected traces keep their
// outcome; missing inventory rows receive synthetic skipped_incomplete results.
void validate_against_inventory(BlockClassification &bc, const std::vector<TraceInventory> &inventory);

}  // namespace mch
