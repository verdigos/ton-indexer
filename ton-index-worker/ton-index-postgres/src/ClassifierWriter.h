#pragma once

#include "ClassifyCore.h"

#include <pqxx/pqxx>

#include <cstdint>
#include <string>
#include <vector>

// Writes classification results to PostgreSQL inside a caller-owned transaction
// that carries no block data. Live mode appends one block's rows and its
// `_blocks_classified` marker; task mode replaces rows of one completion seqno.
// Each trace has its own savepoint.
// SQLSTATE classes 22, 23, 54 and rendering failures get a bounded `unknown`
// rescue plus a `_classifier_failed_traces` record. Persistable empty results
// get the rescue too. Unset rescues are skipped and counted; rejected rescues
// are counted and their failure records have a separate savepoint.
// Capped traces get scalar `unknown` rows and account links from transactions.
// Broken traces are never persisted; lone tick-tock traces have no actions.
// History unavailable, evicted, incomplete and missing-root outcomes suppress
// the marker and record `_classifier_block_issues` for replay by a task worker.
// Connection loss, resource exhaustion, statement timeout, deadlock and schema
// drift propagate to the caller and abort its transaction. The writer never retries.
// A trace-scoped task refuses a seqno with no marker; a whole-seqno task writes
// the marker and clears its issues.
// record_block_issue records a seqno and reason if that pair is absent.
namespace mch {

void classify_lock_blocks(pqxx::work &txn, const std::vector<std::uint32_t> &seqnos);
void record_block_issue(pqxx::work &txn, std::uint32_t mc_seqno, const std::string &reason);

struct ClassifierWriterStats {
  std::uint64_t blocks_written{0}, blocks_already_marked{0}, rows{0}, account_rows{0}, failed_traces{0};
  std::uint64_t rejected_traces{0};
  std::uint64_t blocks_unmarked{0}, rescue_unset{0}, rescue_rejected{0};
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
