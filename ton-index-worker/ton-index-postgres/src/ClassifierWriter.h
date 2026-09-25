#pragma once

#include "ClassifierFailure.h"
#include "ClassifyCore.h"

#include <pqxx/pqxx>

#include <cstdint>
#include <string>
#include <vector>

// Writes live results or task replacements in a caller-owned transaction.
// Each trace uses a savepoint; recoverable insert failures fall back to an
// unknown row. Live blocks with uncovered traces remain unmarked for task replay.
namespace mch {

void classify_lock_blocks(pqxx::dbtransaction &txn, const std::vector<std::uint32_t> &seqnos);
void record_block_issue(pqxx::dbtransaction &txn, std::uint32_t mc_seqno, const std::string &reason);

struct ClassifierWriterStats {
  std::uint64_t blocks_written{0}, blocks_already_marked{0}, rows{0}, account_rows{0}, failed_traces{0};
  std::uint64_t rejected_traces{0};
  std::uint64_t blocks_unmarked{0}, rescue_unset{0}, rescue_rejected{0};
};

void classify_live_write(pqxx::dbtransaction &txn, std::uint32_t mc_seqno,
                         const BlockClassification &bc, ClassifierWriterStats &st);

struct TaskWrite {
  std::uint32_t mc_seqno;
  const BlockClassification *bc;
  bool whole_seqno;
  std::vector<std::int32_t> task_ids;
};

enum class TaskWriteResult { done, ineligible };

TaskWriteResult classify_task_replace(pqxx::dbtransaction &txn, const TaskWrite &w, ClassifierWriterStats &st);

}  // namespace mch
