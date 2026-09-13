#include "ClassifiedSink.h"

#include "ActionJson.h"
#include "td/utils/logging.h"

#include <algorithm>
#include <utility>

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

std::pair<std::uint64_t, std::uint64_t> insert_rows(
    pqxx::dbtransaction &txn, const std::vector<Action> &rows, const ActionRowContext &ctx) {
  std::uint64_t row_count = 0, account_count = 0;
  if (!rows.empty()) {
    row_count = txn.exec(kInsertActions, pqxx::params{action_rows_json(rows, ctx)}).affected_rows();
    for (const auto &row : rows) {
      if (!row.accounts.empty()) {
        account_count += txn.exec(kInsertAccounts, pqxx::params{action_account_rows_json(row, ctx)}).affected_rows();
      }
    }
  }
  return {row_count, account_count};
}

void insert_trace(pqxx::work &txn, const ClassifiedTrace &trace, LiveSinkStats &st) {
  if (!outcome_writes_rows(trace.outcome)) return;
  const auto ctx = row_context(trace.trace);
  try {
    pqxx::subtransaction sub(txn, "trace");
    const auto [rows, accounts] = insert_rows(sub, trace.rows, ctx);
    std::uint64_t failed = 0;
    if (trace.outcome == TraceOutcome::fallback || trace.outcome == TraceOutcome::terminal_failure_empty) {
      failed = sub.exec(kInsertFailure, pqxx::params{ctx.trace_id, trace.failure_reason}).affected_rows();
    }
    sub.commit();
    st.rows += rows;
    st.account_rows += accounts;
    st.failed_traces += failed;
  } catch (const pqxx::sql_error &e) {
    if (!insert_rejected(e.sqlstate())) throw;
    std::string message = e.what();
    message = message.substr(0, message.find_first_of("\r\n"));
    const auto reason = "insert_rejected: " + e.sqlstate() + " " + message;
    try {
      pqxx::subtransaction sub(txn, "rescue");
      const auto [rows, accounts] = insert_rows(sub, {trace.rescue}, ctx);
      const auto failed = sub.exec(kInsertFailure, pqxx::params{ctx.trace_id, reason}).affected_rows();
      sub.commit();
      st.rows += rows;
      st.account_rows += accounts;
      st.failed_traces += failed;
      LOG(WARNING) << "Classification insert rejected trace=" << ctx.trace_id
                   << " sqlstate=" << e.sqlstate() << " message=" << message;
    } catch (const pqxx::sql_error &rescue_error) {
      if (!insert_rejected(rescue_error.sqlstate())) throw;
      std::string rescue_message = rescue_error.what();
      rescue_message = rescue_message.substr(0, rescue_message.find_first_of("\r\n"));
      const auto rescue_reason = "insert_rejected_unknown: " + rescue_error.sqlstate() + " " + rescue_message;
      st.failed_traces += txn.exec(kInsertFailure, pqxx::params{ctx.trace_id, rescue_reason}).affected_rows();
      LOG(ERROR) << "Classification unknown insert rejected trace=" << ctx.trace_id
                 << " sqlstate=" << rescue_error.sqlstate() << " message=" << rescue_message;
    }
    ++st.rejected_traces;
  }
}

}  // namespace

bool insert_rejected(const std::string &sqlstate) {
  return sqlstate.starts_with("22") || sqlstate.starts_with("23");
}

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
