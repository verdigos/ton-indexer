#include "BatchRunner.h"

#include "td/utils/base64.h"
#include "td/utils/logging.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <utility>

BatchRunner::BatchRunner(td::actor::ActorId<DbScanner> db_scanner,
                         td::actor::ActorId<ParseManager> parse_manager,
                         td::actor::ActorId<TraceAssembler> trace_assembler, std::uint32_t read_start,
                         std::uint32_t seqno_from, std::uint32_t seqno_to,
                         std::shared_ptr<std::atomic_int> exit_code, std::shared_ptr<td::Destructor> watcher)
    : db_scanner_(db_scanner)
    , parse_manager_(parse_manager)
    , trace_assembler_(trace_assembler)
    , read_start_(read_start)
    , seqno_from_(seqno_from)
    , seqno_to_(seqno_to)
    , current_seqno_(read_start)
    , exit_code_(std::move(exit_code))
    , watcher_(std::move(watcher)) {
}

void BatchRunner::start_up() {
  td::actor::send_closure(trace_assembler_, &TraceAssembler::reset_state);
  td::actor::send_closure(trace_assembler_, &TraceAssembler::set_expected_seqno, read_start_);
  fetch_seqno();
}

void BatchRunner::fetch_seqno() {
  const auto seqno = current_seqno_;
  auto promise = td::PromiseCreator::lambda(
      [self_id = actor_id(this), seqno](td::Result<schema::MasterchainBlockDataState> result) {
        if (result.is_error()) {
          td::actor::send_closure(self_id, &BatchRunner::fail, seqno, "fetch", result.move_as_error());
          return;
        }
        td::actor::send_closure(self_id, &BatchRunner::seqno_fetched, seqno, result.move_as_ok());
      });
  td::actor::send_closure(db_scanner_, &DbScanner::fetch_seqno, seqno, std::move(promise));
}

void BatchRunner::seqno_fetched(std::uint32_t seqno, schema::MasterchainBlockDataState block_data_state) {
  LOG(DEBUG) << "Fetched seqno " << seqno;

  auto parse_promise = td::PromiseCreator::lambda(
      [self_id = actor_id(this), seqno](td::Result<ParsedBlockPtr> result) {
        if (result.is_error()) {
          td::actor::send_closure(self_id, &BatchRunner::fail, seqno, "parse", result.move_as_error());
          return;
        }
        td::actor::send_closure(self_id, &BatchRunner::seqno_parsed, seqno, result.move_as_ok());
      });

  td::actor::send_closure(
      db_scanner_, &DbScanner::get_cell_db_reader,
      [self_id = actor_id(this), parse_manager = parse_manager_, seqno,
       block_data_state = std::move(block_data_state), parse_promise = std::move(parse_promise)](
          td::Result<std::shared_ptr<vm::CellDbReader>> cell_db_reader) mutable {
        if (cell_db_reader.is_error()) {
          td::actor::send_closure(self_id, &BatchRunner::fail, seqno, "cell DB reader",
                                  cell_db_reader.move_as_error());
          return;
        }
        td::actor::send_closure(parse_manager, &ParseManager::parse, seqno, std::move(block_data_state),
                                cell_db_reader.move_as_ok(), std::move(parse_promise));
      });
}

void BatchRunner::seqno_parsed(std::uint32_t seqno, ParsedBlockPtr parsed_block) {
  LOG(DEBUG) << "Parsed seqno " << seqno;

  auto promise = td::PromiseCreator::lambda([self_id = actor_id(this), seqno](td::Result<ParsedBlockPtr> result) {
    if (result.is_error()) {
      td::actor::send_closure(self_id, &BatchRunner::fail, seqno, "trace assembly", result.move_as_error());
      return;
    }
    td::actor::send_closure(self_id, &BatchRunner::seqno_assembled, seqno, result.move_as_ok());
  });
  td::actor::send_closure(trace_assembler_, &TraceAssembler::assemble, seqno, std::move(parsed_block),
                          std::move(promise));
}

void BatchRunner::seqno_assembled(std::uint32_t seqno, ParsedBlockPtr parsed_block) {
  LOG(DEBUG) << "Assembled traces for seqno " << seqno;

  total_blocks_ += parsed_block->blocks_.size();
  for (const auto &block : parsed_block->blocks_) {
    total_transactions_ += block.transactions.size();
    for (const auto &transaction : block.transactions) {
      transactions_by_trace_[transaction.trace_id].push_back(transaction);
    }
  }
  peak_accumulator_size_ = std::max(peak_accumulator_size_, transactions_by_trace_.size());

  for (const auto &trace : parsed_block->traces_) {
    if (trace.state == schema::Trace::State::complete) {
      transactions_by_trace_.erase(trace.trace_id);
      if (trace.mc_seqno_end >= static_cast<std::int32_t>(seqno_from_) &&
          trace.mc_seqno_end <= static_cast<std::int32_t>(seqno_to_)) {
        ++complete_selected_;
        const auto output_hash = trace.external_hash_norm.value_or(trace.external_hash.value_or(trace.trace_id));
        LOG(INFO) << "Selected complete trace " << td::base64_encode(output_hash.as_slice())
                  << " at masterchain seqno " << trace.mc_seqno_end;
      } else {
        ++complete_out_of_range_;
      }
    } else if (trace.state == schema::Trace::State::broken) {
      ++broken_;
    }
  }

  if (seqno == seqno_to_) {
    finish();
    return;
  }

  ++current_seqno_;
  fetch_seqno();
}

void BatchRunner::fail(std::uint32_t seqno, const char *stage, td::Status error) {
  LOG(ERROR) << "Failed during " << stage << " for seqno " << seqno << ": " << error;
  exit_code_->store(7);
  stop();
}

void BatchRunner::finish() {
  const auto prewarm_seqnos = static_cast<std::uint64_t>(seqno_from_) - read_start_;
  const auto range_seqnos = static_cast<std::uint64_t>(seqno_to_) - seqno_from_ + 1;
  const auto pending_at_eof = transactions_by_trace_.size();

  std::ostringstream summary;
  summary << "ton-mch-index stats: seqnos read prewarm=" << prewarm_seqnos << " range=" << range_seqnos
          << "; blocks parsed=" << total_blocks_ << "; transactions seen=" << total_transactions_
          << "; traces complete-selected=" << complete_selected_
          << " complete-out-of-range=" << complete_out_of_range_ << " pending-at-eof=" << pending_at_eof
          << " broken=" << broken_ << "; peak accumulator size=" << peak_accumulator_size_;

  LOG(INFO) << summary.str();
  std::cout << summary.str() << std::endl;
  exit_code_->store(0);
  stop();
}
