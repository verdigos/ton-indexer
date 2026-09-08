#pragma once

#include "ClassifyCore.h"

#include <pqxx/pqxx>

#include <cstdint>
#include <vector>

namespace mch {

void classify_lock_blocks(pqxx::work &txn, const std::vector<std::uint32_t> &seqnos);

struct LiveSinkStats {
  std::uint64_t blocks_written{0}, blocks_already_marked{0}, rows{0}, account_rows{0}, failed_traces{0};
};

// The caller locks all completion blocks before any batch DML.
void classify_live_write(pqxx::work &txn, std::uint32_t mc_seqno,
                         const BlockClassification &bc, LiveSinkStats &st);

struct TaskWrite {
  std::uint32_t mc_seqno;
  const BlockClassification *bc;
  bool whole_seqno;
  std::vector<std::int32_t> task_ids;
};

enum class TaskWriteResult { done, ineligible };

TaskWriteResult classify_task_replace(pqxx::work &txn, const TaskWrite &w, LiveSinkStats &st);

}  // namespace mch
