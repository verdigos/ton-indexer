#pragma once

#include "DataParser.h"
#include "DbScanner.h"
#include "TraceAssembler.h"

#include "td/actor/actor.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class BatchRunner : public td::actor::Actor {
 public:
  BatchRunner(td::actor::ActorId<DbScanner> db_scanner, td::actor::ActorId<ParseManager> parse_manager,
              td::actor::ActorId<TraceAssembler> trace_assembler, std::uint32_t read_start,
              std::uint32_t seqno_from, std::uint32_t seqno_to, std::shared_ptr<std::atomic_int> exit_code,
              std::shared_ptr<td::Destructor> watcher);

  void start_up() override;

 private:
  void fetch_seqno();
  void seqno_fetched(std::uint32_t seqno, schema::MasterchainBlockDataState block_data_state);
  void seqno_parsed(std::uint32_t seqno, ParsedBlockPtr parsed_block);
  void seqno_assembled(std::uint32_t seqno, ParsedBlockPtr parsed_block);
  void fail(std::uint32_t seqno, const char *stage, td::Status error);
  void finish();

  td::actor::ActorId<DbScanner> db_scanner_;
  td::actor::ActorId<ParseManager> parse_manager_;
  td::actor::ActorId<TraceAssembler> trace_assembler_;
  std::uint32_t read_start_;
  std::uint32_t seqno_from_;
  std::uint32_t seqno_to_;
  std::uint32_t current_seqno_;
  std::shared_ptr<std::atomic_int> exit_code_;
  std::shared_ptr<td::Destructor> watcher_;

  std::unordered_map<td::Bits256, std::vector<schema::Transaction>, Bits256Hasher> transactions_by_trace_;

  // Wave 2 will merge per-seqno rows here with last-write-wins semantics. The map stays empty in
  // wave 1 because this pipeline does not run BlockInterfaceProcessor.
  std::unordered_map<block::StdAddress, std::vector<schema::BlockchainInterfaceV2>> account_interfaces_;

  std::uint64_t total_blocks_{0};
  std::uint64_t total_transactions_{0};
  std::uint64_t complete_selected_{0};
  std::uint64_t complete_out_of_range_{0};
  std::uint64_t broken_{0};
  std::size_t peak_accumulator_size_{0};
};
