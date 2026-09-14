#pragma once

#include "ClassifyCore.h"

#include <pqxx/pqxx>

#include <cstdint>
#include <string>
#include <vector>

// Writes classification results to PostgreSQL inside the caller's transaction:
// live mode appends the rows of a block plus its `_blocks_classified` marker,
// task mode replaces the rows of one completion seqno. One savepoint per trace;
// a trace whose rows PostgreSQL rejects keeps its `unknown` rescue row.
namespace mch {

void classify_lock_blocks(pqxx::work &txn, const std::vector<std::uint32_t> &seqnos);

struct ClassifierWriterStats {
  std::uint64_t blocks_written{0}, blocks_already_marked{0}, rows{0}, account_rows{0}, failed_traces{0};
  std::uint64_t rejected_traces{0};
};

bool insert_rejected(const std::string &sqlstate);

// The caller locks all completion blocks before any batch DML.
void classify_live_write(pqxx::work &txn, std::uint32_t mc_seqno,
                         const BlockClassification &bc, ClassifierWriterStats &st);

struct TaskWrite {
  std::uint32_t mc_seqno;
  const BlockClassification *bc;
  bool whole_seqno;
  std::vector<std::int32_t> task_ids;
};

enum class TaskWriteResult { done, ineligible };

TaskWriteResult classify_task_replace(pqxx::work &txn, const TaskWrite &w, ClassifierWriterStats &st);

}  // namespace mch
