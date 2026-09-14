#pragma once

#include "ClassifierWriter.h"
#include "IndexScheduler.h"
#include "TaskPlanning.h"

#include <array>
#include <deque>

// Claims `_classifier_tasks` rows, replays their seqno window through a task-mode
// ReplayRun and replaces the rows via ClassifierWriter; deferred with backoff on
// refusal or error.
class ClassificationTaskRunner : public td::actor::Actor {
 public:
  ClassificationTaskRunner(std::string connection_string, std::string working_dir,
                           td::actor::ActorId<DbScanner> scanner, td::actor::ActorId<ParseManager> parser,
                           mch::ClassifyPolicy policy, std::shared_ptr<const mch::MchEnginePrep> prep,
                           bool once, double poll_seconds, td::Promise<td::Unit> done)
      : connection_string_(std::move(connection_string)), working_dir_(std::move(working_dir)),
        scanner_(scanner), parser_(parser), policy_(policy), prep_(std::move(prep)),
        once_(once), poll_seconds_(poll_seconds), done_(std::move(done)) {}

  static std::uint64_t enqueue_range(const std::string &connection_string, std::uint32_t from, std::uint32_t to);
  void start_up() override;
  void alarm() override;

 private:
  void claim();
  void prepare_run();
  void oldest_received(td::Result<ton::BlockSeqno> oldest);
  void run_finished(td::Result<mch::RunReport> report);
  void defer(const std::vector<std::int32_t> &ids, bool refused = false);
  void finish_run();
  void print_stats();

  std::string connection_string_, working_dir_, run_dir_;
  td::actor::ActorId<DbScanner> scanner_;
  td::actor::ActorId<ParseManager> parser_;
  mch::ClassifyPolicy policy_;
  std::shared_ptr<const mch::MchEnginePrep> prep_;
  bool once_;
  double poll_seconds_;
  td::Promise<td::Unit> done_;
  td::actor::ActorOwn<IndexScheduler> scheduler_;
  std::deque<mch::TaskRun> pending_;
  std::optional<mch::TaskPlanInput> input_;
  std::map<std::int32_t, unsigned> attempts_;
  std::uint64_t runs_{0}, seqnos_done_{0}, seqnos_ineligible_{0}, malformed_{0}, missing_metadata_{0}, run_errors_{0};
  std::array<std::uint64_t, 4> refusals_{};
  mch::ClassifierWriterStats writer_stats_;
};
