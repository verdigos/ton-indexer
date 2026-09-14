#include "ClassificationTaskRunner.h"

#include "InsertManagerPostgres.h"
#include "version.h"
#include "td/utils/base64.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace {

// Lease duration is ten seconds per seqno in the maximum claim.
constexpr std::uint64_t kLeaseSecondsPerSeqno = 10;
constexpr auto kClaim = R"SQL(
WITH c AS (
  SELECT id, mc_seqno, trace_id FROM _classifier_tasks
  WHERE (start_after IS NULL OR start_after <= now())
    AND (claimed_at IS NULL OR claimed_at < now() - $2::interval)
  ORDER BY mc_seqno, id LIMIT $1 FOR UPDATE SKIP LOCKED)
UPDATE _classifier_tasks t SET claimed_at = now() FROM c WHERE t.id = c.id
RETURNING t.id, t.mc_seqno, t.trace_id
)SQL";
constexpr unsigned kBackoffSeconds = 60, kMaxBackoffSeconds = 3600;
constexpr auto kDefer =
    "UPDATE _classifier_tasks SET claimed_at = NULL, start_after = now() + $2::interval WHERE id = ANY($1)";

unsigned backoff(unsigned attempt) {
  return std::min(kMaxBackoffSeconds, kBackoffSeconds << std::min(attempt, 6u));
}

std::vector<std::int32_t> task_ids(const mch::TaskRun &run) {
  std::vector<std::int32_t> ids;
  for (const auto &task : run.tasks) ids.push_back(task.id);
  return ids;
}

const char *refusal_name(mch::TaskPlanRefusal reason) {
  switch (reason) {
    case mch::TaskPlanRefusal::none: return "none";
    case mch::TaskPlanRefusal::no_partition: return "no_partition";
    case mch::TaskPlanRefusal::inside_live_window: return "inside_live_window";
    case mch::TaskPlanRefusal::history_unavailable: return "history_unavailable";
  }
  return "unknown";
}

}  // namespace

std::uint64_t ClassificationTaskRunner::enqueue_range(const std::string &connection_string, std::uint32_t from, std::uint32_t to) {
  if (from == 0 || from > to || to >= INT32_MAX) throw std::runtime_error("Invalid task range");
  pqxx::connection c(connection_string);
  pqxx::work txn(c);
  // Serialize enqueue utilities because the queue has no unique seqno key.
  txn.exec("LOCK TABLE _classifier_tasks IN SHARE ROW EXCLUSIVE MODE");
  const auto count = txn.exec(
      "INSERT INTO _classifier_tasks(mc_seqno) SELECT k FROM generate_series($1::int, $2::int) AS s(k) "
      "WHERE NOT EXISTS (SELECT 1 FROM _classifier_tasks t WHERE t.mc_seqno = k AND t.claimed_at IS NULL)",
      pqxx::params{from, to}).affected_rows();
  txn.commit();
  return count;
}

void ClassificationTaskRunner::start_up() {
  try {
    const auto version = get_current_db_version(connection_string_);
    if (!version || !(*version == latest_version)) {
      throw std::runtime_error("Task mode requires database version " + latest_version.str());
    }
    alarm_timestamp() = td::Timestamp::now();
  } catch (const std::exception &e) {
    done_.set_error(td::Status::Error(td::Slice(e.what())));
    stop();
  }
}

void ClassificationTaskRunner::alarm() {
  if (pending_.empty()) claim();
  else prepare_run();
}

void ClassificationTaskRunner::claim() {
  try {
    pqxx::connection c(connection_string_);
    pqxx::work txn(c);
    const auto rows = txn.exec(kClaim, pqxx::params{
        policy_.run_max_seqnos, std::to_string(policy_.run_max_seqnos * kLeaseSecondsPerSeqno) + " seconds"});
    std::vector<mch::ClaimedTask> tasks;
    for (const auto &row : rows) {
      mch::ClaimedTask task{row[0].as<std::int32_t>(), std::nullopt, std::nullopt};
      if (row[1].is_null() && row[2].is_null()) {
        txn.exec("DELETE FROM _classifier_tasks WHERE id = $1", pqxx::params{task.id});
        ++malformed_;
        continue;
      }
      if (!row[1].is_null()) {
        const auto k = row[1].as<std::int64_t>();
        if (k > 0 && k < INT32_MAX) {
          task.mc_seqno = static_cast<std::uint32_t>(k);
          tasks.push_back(std::move(task));
          continue;
        }
      } else {
        const auto id = row[2].as<std::string>();
        auto decoded = td::base64_decode(id);
        const auto metadata = txn.exec(
            "SELECT mc_seqno_start, mc_seqno_end FROM traces WHERE trace_id = $1 AND state = 'complete'",
            pqxx::params{id});
        if (decoded.is_ok() && decoded.ok().size() == 32 && metadata.size() == 1 &&
            !metadata[0][0].is_null() && !metadata[0][1].is_null()) {
          const auto start = metadata[0][0].as<std::int64_t>();
          const auto end = metadata[0][1].as<std::int64_t>();
          if (start > 0 && start <= end && end < INT32_MAX) {
            task.trace_id.emplace();
            task.trace_id->as_slice().copy_from(td::Slice(decoded.ok()));
            task.start = static_cast<std::uint32_t>(start);
            task.end = static_cast<std::uint32_t>(end);
            tasks.push_back(std::move(task));
            continue;
          }
        }
      }
      ++missing_metadata_;
      auto &attempt = attempts_[task.id];
      txn.exec(kDefer, pqxx::params{std::vector<std::int32_t>{task.id}, std::to_string(backoff(attempt)) + " seconds"});
      attempt = std::min(attempt + 1, 6u);
      LOG(WARNING) << "Task " << task.id << ": missing_metadata";
    }
    txn.commit();
    for (auto &run : mch::group_consecutive(std::move(tasks))) pending_.push_back(std::move(run));
    if (rows.empty()) {
      print_stats();
      if (once_) {
        done_.set_value(td::Unit());
        stop();
        return;
      }
      alarm_timestamp() = td::Timestamp::in(poll_seconds_);
    } else {
      alarm_timestamp() = td::Timestamp::now();
    }
  } catch (const std::exception &e) {
    ++missing_metadata_;
    LOG(WARNING) << "Task claim failed: " << e.what();
    alarm_timestamp() = td::Timestamp::in(poll_seconds_);
  }
}

void ClassificationTaskRunner::prepare_run() {
  const auto &run = pending_.front();
  try {
    pqxx::connection c(connection_string_);
    std::vector<mch::TraceInventory> inventory;
    for (const auto &row : InsertManagerPostgres::read_trace_inventory(c, run.from, run.to)) {
      if (!run.trace_filter || row.trace_id == *run.trace_filter) {
        inventory.push_back({row.trace_id, row.mc_seqno_start, row.mc_seqno_end, row.nodes});
      }
    }
    // A resolved trace task must still exist in the completion inventory.
    for (const auto &task : run.tasks) {
      if (!task.mc_seqno && std::none_of(inventory.begin(), inventory.end(), [&](const auto &row) {
            return row.trace_id == *task.trace_id && row.mc_seqno_start == task.start && row.mc_seqno_end == task.end;
          })) throw std::runtime_error("Trace task is absent or changed in inventory");
    }
    const auto partitioned = PartitionManagerPostgres::get_action_seqno_bounds(c);
    pqxx::read_transaction txn(c);
    const auto frontier = txn.exec("SELECT finalized_mc_seqno FROM _ton_indexer_progress WHERE id = 1");
    if (frontier.size() != 1 || frontier[0][0].is_null()) throw std::runtime_error("Missing live frontier");
    const auto live = frontier[0][0].as<std::uint32_t>();
    input_ = mch::TaskPlanInput{run.from, run.to, std::move(inventory), 0, partitioned, live, run.trace_filter};
    auto promise = td::PromiseCreator::lambda([self = actor_id(this)](td::Result<ton::BlockSeqno> oldest) {
      td::actor::send_closure(self, &ClassificationTaskRunner::oldest_received, std::move(oldest));
    });
    td::actor::send_closure(scanner_, &DbScanner::get_oldest_mc_seqno, std::move(promise));
  } catch (const std::exception &e) {
    ++missing_metadata_;
    LOG(WARNING) << "Task run " << run.from << ".." << run.to << ": missing_metadata: " << e.what();
    defer(task_ids(run));
    finish_run();
  }
}

void ClassificationTaskRunner::oldest_received(td::Result<ton::BlockSeqno> oldest) {
  const auto &run = pending_.front();
  if (oldest.is_error()) {
    ++refusals_[static_cast<std::size_t>(mch::TaskPlanRefusal::history_unavailable)];
    LOG(WARNING) << "Task run " << run.from << ".." << run.to << ": history_unavailable: " << oldest.error();
    defer(task_ids(run), true);
    finish_run();
    return;
  }
  input_->oldest_available = oldest.move_as_ok();
  auto plan = mch::plan_task_run(*input_, policy_);
  if (plan.refusal != mch::TaskPlanRefusal::none) {
    ++refusals_[static_cast<std::size_t>(plan.refusal)];
    LOG(WARNING) << "Task run " << run.from << ".." << run.to << ": " << refusal_name(plan.refusal);
    defer(task_ids(run), true);
    finish_run();
    return;
  }
  try {
    run_dir_ = working_dir_ + "/task-" + std::to_string(run.from) + "-" + std::to_string(run.to);
    std::filesystem::remove_all(run_dir_);
    std::filesystem::create_directories(run_dir_);
    plan.run.state_dir = run_dir_;
    auto promise = td::PromiseCreator::lambda([self = actor_id(this)](td::Result<mch::RunReport> report) {
      td::actor::send_closure(self, &ClassificationTaskRunner::run_finished, std::move(report));
    });
    scheduler_ = td::actor::create_actor<IndexScheduler>(
        "taskrun-" + std::to_string(run.from) + "-" + std::to_string(run.to), scanner_,
        td::actor::ActorId<InsertManagerInterface>{}, parser_, run_dir_, 0, 0, false, 32,
        QueueState{30000, 30000, 500000, 500000}, 10, nullptr, 0, std::string{},
        std::optional<mch::ReplayRun>{std::move(plan.run)}, prep_, std::move(promise));
    ++runs_;
    td::actor::send_closure(scheduler_, &IndexScheduler::run);
  } catch (const std::exception &e) {
    run_finished(td::Status::Error(td::Slice(e.what())));
  }
}

void ClassificationTaskRunner::run_finished(td::Result<mch::RunReport> result) {
  const auto &run = pending_.front();
  if (result.is_error()) {
    ++run_errors_;
    LOG(WARNING) << "Task run " << run.from << ".." << run.to << " failed: " << result.error();
    defer(task_ids(run));
    finish_run();
    return;
  }
  auto report = result.move_as_ok();
  for (auto k = run.from; k <= run.to; ++k) {
    std::vector<std::int32_t> ids;
    bool whole_seqno = false;
    for (const auto &task : run.tasks) {
      if (task.mc_seqno.value_or(task.end) == k) {
        ids.push_back(task.id);
        whole_seqno |= task.mc_seqno.has_value();
      }
    }
    if (ids.empty()) continue;
    try {
      auto &bc = report.blocks[k];
      std::vector<mch::TraceInventory> inventory;
      for (const auto &row : input_->inventory) if (row.mc_seqno_end == k) inventory.push_back(row);
      mch::validate_against_inventory(bc, inventory);
      pqxx::connection c(connection_string_);
      pqxx::work txn(c);
      auto stats = writer_stats_;
      if (mch::classify_task_replace(txn, {k, &bc, whole_seqno, ids}, stats) == mch::TaskWriteResult::done) {
        txn.commit();
        writer_stats_ = stats;
        ++seqnos_done_;
        for (const auto id : ids) attempts_.erase(id);
      } else {
        txn.abort();
        ++seqnos_ineligible_;
        defer(ids);
      }
    } catch (const std::exception &e) {
      ++run_errors_;
      LOG(WARNING) << "Task completion " << k << " failed: " << e.what();
      defer(ids);
    }
  }
  finish_run();
}

void ClassificationTaskRunner::defer(const std::vector<std::int32_t> &ids, bool refused) {
  try {
    pqxx::connection c(connection_string_);
    pqxx::work txn(c);
    for (const auto id : ids) {
      const auto seconds = refused ? kMaxBackoffSeconds : backoff(attempts_[id]);
      txn.exec(kDefer, pqxx::params{std::vector<std::int32_t>{id}, std::to_string(seconds) + " seconds"});
    }
    txn.commit();
    if (!refused) for (const auto id : ids) attempts_[id] = std::min(attempts_[id] + 1, 6u);
  } catch (const std::exception &e) {
    LOG(WARNING) << "Task deferral failed; rows retained under claim lease: " << e.what();
  }
}

void ClassificationTaskRunner::finish_run() {
  scheduler_.reset();
  if (!run_dir_.empty()) {
    std::error_code error;
    std::filesystem::remove_all(run_dir_, error);
    if (error) LOG(WARNING) << "Cannot remove " << run_dir_ << ": " << error.message();
    run_dir_.clear();
  }
  input_.reset();
  pending_.pop_front();
  alarm_timestamp() = td::Timestamp::now();
}

void ClassificationTaskRunner::print_stats() {
  LOG(INFO) << "MCH tasks: runs=" << runs_ << " seqnos_done=" << seqnos_done_
            << " seqnos_ineligible=" << seqnos_ineligible_ << " malformed=" << malformed_
            << " missing_metadata=" << missing_metadata_ << " run_errors=" << run_errors_
            << " no_partition=" << refusals_[static_cast<std::size_t>(mch::TaskPlanRefusal::no_partition)]
            << " inside_live_window=" << refusals_[static_cast<std::size_t>(mch::TaskPlanRefusal::inside_live_window)]
            << " history_unavailable=" << refusals_[static_cast<std::size_t>(mch::TaskPlanRefusal::history_unavailable)]
            << " rows=" << writer_stats_.rows << " account_rows=" << writer_stats_.account_rows
            << " rejected=" << writer_stats_.rejected_traces;
}
