#include "ClassifiedSink.h"

#include "ActionJson.h"

#include <algorithm>

namespace mch {
namespace {

constexpr std::int64_t kClassifyLockBase = 0x4d43480000000000;
constexpr auto kLockBlock = "SELECT pg_advisory_xact_lock($1::int8)";
constexpr auto kHasMarker = "SELECT 1 FROM _blocks_classified WHERE mc_seqno = $1";
constexpr auto kInsertActions =
    "INSERT INTO actions SELECT * FROM jsonb_populate_recordset(NULL::actions, $1::jsonb) "
    "ON CONFLICT DO NOTHING";
constexpr auto kInsertAccounts =
    "INSERT INTO action_accounts SELECT * FROM jsonb_populate_recordset(NULL::action_accounts, $1::jsonb) "
    "ON CONFLICT DO NOTHING";
constexpr auto kInsertFailure =
    "INSERT INTO _classifier_failed_traces(trace_id, broken, error) VALUES ($1, false, $2)";
constexpr auto kInsertMarker =
    "INSERT INTO _blocks_classified(mc_seqno) VALUES ($1) ON CONFLICT DO NOTHING";
constexpr auto kDeleteActions = "DELETE FROM actions WHERE trace_id = $1 AND trace_mc_seqno_end = $2";
constexpr auto kDeleteAccounts = "DELETE FROM action_accounts WHERE trace_id = $1 AND trace_mc_seqno_end = $2";
constexpr auto kDeleteTasks = "DELETE FROM _classifier_tasks WHERE id = ANY($1)";

void insert_trace(pqxx::work &txn, const ClassifiedTrace &trace, LiveSinkStats &st) {
  if (!outcome_writes_rows(trace.outcome)) return;
  const auto ctx = row_context(trace.trace);
  if (!trace.rows.empty()) {
    st.rows += txn.exec(kInsertActions, pqxx::params{action_rows_json(trace.rows, ctx)}).affected_rows();
    for (const auto &row : trace.rows) {
      if (!row.accounts.empty()) {
        st.account_rows += txn.exec(kInsertAccounts, pqxx::params{action_account_rows_json(row, ctx)}).affected_rows();
      }
    }
  }
  if (trace.outcome == TraceOutcome::fallback || trace.outcome == TraceOutcome::terminal_failure_empty) {
    st.failed_traces += txn.exec(kInsertFailure, pqxx::params{ctx.trace_id, trace.failure_reason}).affected_rows();
  }
}

}  // namespace

void classify_lock_blocks(pqxx::work &txn, const std::vector<std::uint32_t> &seqnos) {
  auto sorted = seqnos;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  for (const auto seqno : sorted) {
    txn.exec(kLockBlock, pqxx::params{kClassifyLockBase + seqno});
  }
}

void classify_live_write(pqxx::work &txn, std::uint32_t mc_seqno,
                         const BlockClassification &bc, LiveSinkStats &st) {
  if (!txn.exec(kHasMarker, pqxx::params{mc_seqno}).empty()) {
    ++st.blocks_already_marked;
    return;
  }
  for (const auto &trace : bc.traces) insert_trace(txn, trace, st);
  st.blocks_written += txn.exec(kInsertMarker, pqxx::params{mc_seqno}).affected_rows();
}

TaskWriteResult classify_task_replace(pqxx::work &txn, const TaskWrite &w, LiveSinkStats &st) {
  classify_lock_blocks(txn, {w.mc_seqno});
  if (!task_eligible(*w.bc)) return TaskWriteResult::ineligible;
  for (const auto &trace : w.bc->traces) {
    const auto ctx = row_context(trace.trace);
    txn.exec(kDeleteActions, pqxx::params{ctx.trace_id, w.mc_seqno});
    txn.exec(kDeleteAccounts, pqxx::params{ctx.trace_id, w.mc_seqno});
    insert_trace(txn, trace, st);
  }
  if (w.whole_seqno) {
    st.blocks_written += txn.exec(kInsertMarker, pqxx::params{w.mc_seqno}).affected_rows();
  }
  txn.exec(kDeleteTasks, pqxx::params{w.task_ids});
  return TaskWriteResult::done;
}

}  // namespace mch
