#pragma once

#include "DataParser.h"
#include "DbScanner.h"
#include "TraceAssembler.h"

#include "ClassifyCore.h"
#include "EnginePrep.h"
#include "ParsedBlockLookupSource.h"

#include "td/actor/actor.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class BatchRunner : public td::actor::Actor {
 public:
  BatchRunner(td::actor::ActorId<DbScanner> db_scanner, td::actor::ActorId<ParseManager> parse_manager,
              td::actor::ActorId<TraceAssembler> trace_assembler,
              std::shared_ptr<const mch::MchEnginePrep> prep, std::string output_path,
              std::uint32_t read_start, std::uint32_t seqno_from, std::uint32_t seqno_to,
              bool tier2_enabled, bool emit_tick_tock, bool tier1_completion_only,
              std::shared_ptr<std::atomic_int> exit_code, std::shared_ptr<td::Destructor> watcher);

  void start_up() override;

 private:
  void fetch_seqno();
  void seqno_fetched(std::uint32_t seqno, schema::MasterchainBlockDataState block_data_state);
  void seqno_parsed(std::uint32_t seqno, ParsedBlockPtr parsed_block);
  void seqno_traces_assembled(std::uint32_t seqno, ParsedBlockPtr parsed_block);
  void seqno_interfaces_processed(std::uint32_t seqno, ParsedBlockPtr parsed_block);
  void classify_trace(const std::string &trace_key, std::vector<schema::Transaction> txs,
                      const ParsedBlockPtr &parsed_block);
  void fail(std::uint32_t seqno, const char *stage, td::Status error);
  void finish();

  td::actor::ActorId<DbScanner> db_scanner_;
  td::actor::ActorId<ParseManager> parse_manager_;
  td::actor::ActorId<TraceAssembler> trace_assembler_;
  std::shared_ptr<const mch::MchEnginePrep> prep_;
  std::string output_path_;
  std::ofstream output_;
  std::uint32_t read_start_;
  std::uint32_t seqno_from_;
  std::uint32_t seqno_to_;
  std::uint32_t current_seqno_;
  bool tier2_enabled_;
  bool emit_tick_tock_;
  bool tier1_completion_only_;
  std::size_t tick_tock_skipped_{0};
  // Wall-clock accumulators per stage (seconds).
  double stage_started_{0};
  double t_fetch_{0}, t_parse_{0}, t_assemble_{0}, t_interfaces_{0}, t_classify_{0};
  double run_started_{0};
  std::shared_ptr<std::atomic_int> exit_code_;
  std::shared_ptr<td::Destructor> watcher_;

  std::unordered_map<td::Bits256, std::vector<schema::Transaction>, Bits256Hasher> transactions_by_trace_;

  std::unordered_map<block::StdAddress, std::vector<schema::BlockchainInterfaceV2>> account_interfaces_;

  std::uint64_t total_blocks_{0};
  std::uint64_t total_transactions_{0};
  std::uint64_t complete_selected_{0};
  std::uint64_t complete_out_of_range_{0};
  std::uint64_t broken_{0};
  std::size_t peak_accumulator_size_{0};
  std::uint64_t classified_ok_{0};
  std::uint64_t classified_failed_{0};
  std::map<mch::FailureCategory, std::uint64_t> classification_failures_by_category_;
  std::uint64_t total_actions_emitted_{0};
  mch::ParsedBlockLookupSource::LookupStats lookup_stats_;
  std::vector<std::string> failed_trace_keys_;
};
