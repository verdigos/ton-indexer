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

std::vector<TaskRun> group_consecutive(std::vector<ClaimedTask> tasks);
void validate_against_inventory(BlockClassification &bc, const std::vector<TraceInventory> &inventory);

}  // namespace mch
