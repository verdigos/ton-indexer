#include "ClassifierWriter.h"

#include "ActionJson.h"
#include "td/utils/logging.h"

#include <algorithm>
#include <new>
#include <set>
#include <stdexcept>
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
constexpr auto kInsertIssue =
    "INSERT INTO _classifier_block_issues(mc_seqno, reason) SELECT $1::integer, $2::varchar "
    "WHERE NOT EXISTS (SELECT 1 FROM _classifier_block_issues WHERE mc_seqno = $1 AND reason = $2) "
    "AND NOT EXISTS (SELECT 1 FROM _blocks_classified WHERE mc_seqno = $1)";
constexpr auto kDeleteIssues = "DELETE FROM _classifier_block_issues WHERE mc_seqno = $1";

bool recoverable_skip(TraceOutcome outcome) {
  return outcome == TraceOutcome::history_unavailable || outcome == TraceOutcome::skipped_evicted ||
         outcome == TraceOutcome::skipped_incomplete || outcome == TraceOutcome::skipped_missing_root;
}

std::pair<std::uint64_t, std::uint64_t> insert_capped(
    pqxx::dbtransaction &txn, const schema::Trace &trace, const ActionRowContext &ctx) {
  const auto rows = txn.exec(
      "INSERT INTO actions (trace_id, action_id, type, start_lt, end_lt, start_utime, end_utime, "
      "mc_seqno_end, trace_end_lt, trace_end_utime, trace_mc_seqno_end, trace_external_hash, "
      "trace_external_hash_norm, tx_hashes, ancestor_type, success, payload) "
      "VALUES ($1::tonhash, $1::tonhash, 'unknown', $2::bigint, $3::bigint, $4::integer, $5::integer, "
      "$6::integer, $3::bigint, $5::integer, $6::integer, $7::tonhash, $8::tonhash, '{}', '{}', "
      "(SELECT bool_and(NOT coalesce(aborted, false)) FROM transactions WHERE trace_id = $1::tonhash "
      "AND mc_block_seqno BETWEEN $9::integer AND $10::integer), decode('0180', 'hex')) "
      "ON CONFLICT DO NOTHING",
      pqxx::params{ctx.trace_id, trace.start_lt, ctx.trace_end_lt, trace.start_utime,
                   ctx.trace_end_utime, ctx.trace_mc_seqno_end, ctx.trace_external_hash,
                   ctx.trace_external_hash_norm, trace.mc_seqno_start, trace.mc_seqno_end}).affected_rows();
  const auto accounts = txn.exec(
      "INSERT INTO action_accounts(action_id, trace_id, account, trace_end_lt, action_end_lt, "
      "trace_end_utime, trace_mc_seqno_end, action_end_utime) "
      "SELECT DISTINCT $1::tonhash, $1::tonhash, account, $2::bigint, $3::bigint, $4::integer, $5::integer, "
      "$6::bigint FROM transactions WHERE trace_id = $1::tonhash "
      "AND mc_block_seqno BETWEEN $7::integer AND $8::integer "
      "ON CONFLICT DO NOTHING",
      pqxx::params{ctx.trace_id, ctx.trace_end_lt, ctx.trace_end_lt, ctx.trace_end_utime,
                   ctx.trace_mc_seqno_end, ctx.trace_end_utime, trace.mc_seqno_start, trace.mc_seqno_end}).affected_rows();
  return {rows, accounts};
}

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

bool insert_trace(pqxx::dbtransaction &txn, const TraceClassificationResult &trace, ClassifierWriterStats &st) {
  const bool capped = trace.outcome == TraceOutcome::skipped_capped;
  if (!should_persist_outcome(trace.outcome) && !capped) return true;
  if (!capped && trace.rows.empty() && trace.rescue.action_id.empty()) {
    ++st.rescue_unset;
    return false;
  }
  std::string reason;
  const auto ctx = row_context(trace.trace);
  try {
    pqxx::subtransaction sub(txn, "trace");
    const auto [rows, accounts] = capped ? insert_capped(sub, trace.trace, ctx)
        : trace.rows.empty() ? insert_rows(sub, {trace.rescue}, ctx) : insert_rows(sub, trace.rows, ctx);
    if (capped && accounts == 0 &&
        sub.exec("SELECT 1 FROM action_accounts WHERE trace_id = $1 AND trace_mc_seqno_end = $2 LIMIT 1",
                 pqxx::params{ctx.trace_id, ctx.trace_mc_seqno_end}).empty()) {
      throw std::runtime_error("no_account_links");
    }
    std::uint64_t failed = 0;
    if (trace.outcome == TraceOutcome::fallback || trace.outcome == TraceOutcome::terminal_failure_empty) {
      failed = sub.exec(kInsertFailure, pqxx::params{ctx.trace_id,
          trace.failure_reason.substr(0, trace.failure_reason.find_first_of("\r\n")).substr(0, 200)}).affected_rows();
    }
    sub.commit();
    st.rows += rows;
    st.account_rows += accounts;
    st.failed_traces += failed;
    return true;
  } catch (const pqxx::broken_connection &) {
    throw;
  } catch (const pqxx::in_doubt_error &) {
    throw;
  } catch (const std::bad_alloc &) {
    throw;
  } catch (const pqxx::sql_error &e) {
    if (!insert_rejected(e.sqlstate())) throw;
    reason = "insert_rejected: " + e.sqlstate() + " " + e.what();
  } catch (const std::exception &e) {
    reason = std::string("insert_failed: ") + e.what();
  }
  reason = reason.substr(0, reason.find_first_of("\r\n")).substr(0, 200);
  LOG(WARNING) << "Classification insert failed: " << reason;
  ++st.rejected_traces;
  if (!capped && trace.rescue.action_id.empty()) {
    ++st.rescue_unset;
    return false;
  }
  if (!capped && !trace.rows.empty()) {
    try {
      pqxx::subtransaction sub(txn, "rescue");
      const auto [rows, accounts] = insert_rows(sub, {trace.rescue}, ctx);
      const auto failed = sub.exec(kInsertFailure, pqxx::params{ctx.trace_id, reason}).affected_rows();
      sub.commit();
      st.rows += rows;
      st.account_rows += accounts;
      st.failed_traces += failed;
      return true;
    } catch (const pqxx::broken_connection &) {
      throw;
    } catch (const pqxx::in_doubt_error &) {
      throw;
    } catch (const std::bad_alloc &) {
      throw;
    } catch (const pqxx::sql_error &e) {
      if (!insert_rejected(e.sqlstate())) throw;
      reason = "insert_rejected_unknown: " + e.sqlstate() + " " + e.what();
    } catch (const std::exception &e) {
      reason = std::string("insert_failed_unknown: ") + e.what();
    }
  }
  ++st.rescue_rejected;
  reason = reason.substr(0, reason.find_first_of("\r\n")).substr(0, 200);
  LOG(ERROR) << "Classification unknown insert failed: " << reason;
  try {
    pqxx::subtransaction sub(txn, "failure_record");
    const auto failed = sub.exec(kInsertFailure, pqxx::params{ctx.trace_id, reason}).affected_rows();
    sub.commit();
    st.failed_traces += failed;
  } catch (const pqxx::broken_connection &) {
    throw;
  } catch (const pqxx::in_doubt_error &) {
    throw;
  } catch (const std::bad_alloc &) {
    throw;
  } catch (const pqxx::sql_error &e) {
    if (!insert_rejected(e.sqlstate())) throw;
    LOG(ERROR) << "Classification failure record rejected: " << e.what();
  } catch (const std::exception &e) {
    LOG(ERROR) << "Classification failure record failed: " << e.what();
  }
  return false;
}

}  // namespace

bool insert_rejected(const std::string &sqlstate) {
  return sqlstate.starts_with("22") || sqlstate.starts_with("23") || sqlstate.starts_with("54");
}

void classify_lock_blocks(pqxx::dbtransaction &txn, const std::vector<std::uint32_t> &seqnos) {
  auto sorted = seqnos;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  for (const auto seqno : sorted) {
    txn.exec(kLockBlock, pqxx::params{kClassifyLockBase + seqno});
  }
}

void record_block_issue(pqxx::dbtransaction &txn, std::uint32_t mc_seqno, const std::string &reason) {
  txn.exec(kInsertIssue, pqxx::params{mc_seqno, reason});
}

void classify_live_write(pqxx::dbtransaction &txn, std::uint32_t mc_seqno,
                         const BlockClassification &bc, ClassifierWriterStats &st) {
  if (!txn.exec(kHasMarker, pqxx::params{mc_seqno}).empty()) {
    ++st.blocks_already_marked;
    return;
  }
  std::set<std::string> issues;
  for (const auto &trace : bc.traces) {
    const auto rescue_unset = st.rescue_unset;
    if (!insert_trace(txn, trace, st)) {
      issues.insert(st.rescue_unset != rescue_unset ? "rescue_unset" : "rescue_rejected");
    }
    if (recoverable_skip(trace.outcome)) issues.insert(trace_outcome_name(trace.outcome));
  }
  if (!issues.empty()) {
    for (const auto &reason : issues) {
      record_block_issue(txn, mc_seqno, reason);
    }
    ++st.blocks_unmarked;
    return;
  }
  st.blocks_written += txn.exec(kInsertMarker, pqxx::params{mc_seqno}).affected_rows();
}

TaskWriteResult classify_task_replace(pqxx::dbtransaction &txn, const TaskWrite &w, ClassifierWriterStats &st) {
  classify_lock_blocks(txn, {w.mc_seqno});
  if (!w.whole_seqno && txn.exec(kHasMarker, pqxx::params{w.mc_seqno}).empty()) {
    return TaskWriteResult::ineligible;
  }
  if (!outcomes_allow_replacement(*w.bc)) return TaskWriteResult::ineligible;
  for (const auto &trace : w.bc->traces) {
    const auto ctx = row_context(trace.trace);
    txn.exec(kDeleteActions, pqxx::params{ctx.trace_id, w.mc_seqno});
    txn.exec(kDeleteAccounts, pqxx::params{ctx.trace_id, w.mc_seqno});
    if (!insert_trace(txn, trace, st)) {
      throw std::runtime_error("rescue rejected for trace " + ctx.trace_id);
    }
  }
  if (w.whole_seqno) {
    txn.exec(kDeleteIssues, pqxx::params{w.mc_seqno});
    st.blocks_written += txn.exec(kInsertMarker, pqxx::params{w.mc_seqno}).affected_rows();
  }
  txn.exec(kDeleteTasks, pqxx::params{w.task_ids});
  return TaskWriteResult::done;
}

}  // namespace mch
