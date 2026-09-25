#include "BatchRunner.h"

#include "td/utils/Time.h"

#include <cstdio>

#include "BlockInterfacesDetector.h"
#include "Classify.h"
#include "RedisActionSerialize.h"
#include "emu/EmuTypes.h"

#include "td/utils/JsonBuilder.h"
#include "td/utils/base64.h"
#include "td/utils/logging.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <utility>

BatchRunner::BatchRunner(td::actor::ActorId<DbScanner> db_scanner,
                         td::actor::ActorId<ParseManager> parse_manager,
                         td::actor::ActorId<TraceAssembler> trace_assembler,
                         std::shared_ptr<const mch::MchEnginePrep> prep, std::string output_path,
                         std::uint32_t read_start, std::uint32_t seqno_from, std::uint32_t seqno_to,
                         bool tier2_enabled, bool emit_tick_tock,
                         std::shared_ptr<std::atomic_int> exit_code, std::shared_ptr<td::Destructor> watcher)
    : db_scanner_(db_scanner)
    , parse_manager_(parse_manager)
    , trace_assembler_(trace_assembler)
    , prep_(std::move(prep))
    , output_path_(std::move(output_path))
    , read_start_(read_start)
    , seqno_from_(seqno_from)
    , seqno_to_(seqno_to)
    , current_seqno_(read_start)
    , tier2_enabled_(tier2_enabled)
    , emit_tick_tock_(emit_tick_tock)
    , exit_code_(std::move(exit_code))
    , watcher_(std::move(watcher)) {
}

namespace {
double now_s() {
  return td::Time::now();
}
}  // namespace

void BatchRunner::start_up() {
  output_.open(output_path_, std::ios::out | std::ios::trunc);
  if (!output_) {
    LOG(ERROR) << "Failed to open output file " << output_path_;
    exit_code_->store(7);
    stop();
    return;
  }

  td::actor::send_closure(trace_assembler_, &TraceAssembler::reset_state);
  td::actor::send_closure(trace_assembler_, &TraceAssembler::set_expected_seqno, read_start_);
  run_started_ = now_s();
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
  stage_started_ = now_s();
  td::actor::send_closure(db_scanner_, &DbScanner::fetch_seqno, seqno, std::move(promise));
}

void BatchRunner::seqno_fetched(std::uint32_t seqno, schema::MasterchainBlockDataState block_data_state) {
  t_fetch_ += now_s() - stage_started_;
  stage_started_ = now_s();
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
  t_parse_ += now_s() - stage_started_;
  stage_started_ = now_s();
  LOG(DEBUG) << "Parsed seqno " << seqno;

  auto promise = td::PromiseCreator::lambda([self_id = actor_id(this), seqno](td::Result<ParsedBlockPtr> result) {
    if (result.is_error()) {
      td::actor::send_closure(self_id, &BatchRunner::fail, seqno, "trace assembly", result.move_as_error());
      return;
    }
    td::actor::send_closure(self_id, &BatchRunner::seqno_traces_assembled, seqno, result.move_as_ok());
  });
  td::actor::send_closure(trace_assembler_, &TraceAssembler::assemble, seqno, std::move(parsed_block),
                          std::move(promise));
}

void BatchRunner::seqno_traces_assembled(std::uint32_t seqno, ParsedBlockPtr parsed_block) {
  t_assemble_ += now_s() - stage_started_;
  stage_started_ = now_s();
  LOG(DEBUG) << "Assembled traces for seqno " << seqno;

  auto promise = td::PromiseCreator::lambda([self_id = actor_id(this), seqno](td::Result<ParsedBlockPtr> result) {
    if (result.is_error()) {
      td::actor::send_closure(self_id, &BatchRunner::fail, seqno, "interface detection", result.move_as_error());
      return;
    }
    td::actor::send_closure(self_id, &BatchRunner::seqno_interfaces_processed, seqno, result.move_as_ok());
  });
  td::actor::create_actor<BlockInterfaceProcessor>("BlockInterfaceProcessor", std::move(parsed_block),
                                                    std::move(promise))
      .release();
}

void BatchRunner::seqno_interfaces_processed(std::uint32_t seqno, ParsedBlockPtr parsed_block) {
  t_interfaces_ += now_s() - stage_started_;
  stage_started_ = now_s();
  LOG(DEBUG) << "Interfaces processed for seqno " << seqno;

  total_blocks_ += parsed_block->blocks_.size();
  for (const auto &block : parsed_block->blocks_) {
    total_transactions_ += block.transactions.size();
    for (const auto &transaction : block.transactions) {
      auto &payload = payloads_by_trace_[transaction.trace_id];
      payload.txs.push_back(transaction);
      mch::merge_trace_interface_memo(payload.memo, transaction, parsed_block->account_interfaces_);
    }
  }
  peak_accumulator_size_ = std::max(peak_accumulator_size_, payloads_by_trace_.size());

  for (const auto &trace : parsed_block->traces_) {
    if (trace.state == schema::Trace::State::complete) {
      auto it = payloads_by_trace_.find(trace.trace_id);
      TracePayload payload;
      if (it != payloads_by_trace_.end()) {
        payload = std::move(it->second);
        payloads_by_trace_.erase(it);
      }
      if (trace.mc_seqno_end >= static_cast<std::int32_t>(seqno_from_) &&
          trace.mc_seqno_end <= static_cast<std::int32_t>(seqno_to_)) {
        if (!trace.external_hash.has_value() && !emit_tick_tock_) {
          // Tick-tock traces are omitted by default; --emit-tick-tock restores them.
          ++tick_tock_skipped_;
          continue;
        }
        ++complete_selected_;
        const auto output_hash = trace.trace_id;
        const std::string trace_key = td::base64_encode(output_hash.as_slice());
        LOG(INFO) << "Selected complete trace " << trace_key
                  << " at masterchain seqno " << trace.mc_seqno_end;
        std::stable_sort(payload.txs.begin(), payload.txs.end(),
                         [](const auto &left, const auto &right) { return left.lt < right.lt; });
        const double classify_started = now_s();
        classify_trace(trace_key, std::move(payload.txs), payload.memo, parsed_block);
        t_classify_ += now_s() - classify_started;
      } else {
        ++complete_out_of_range_;
      }
    } else if (trace.state == schema::Trace::State::broken) {
      payloads_by_trace_.erase(trace.trace_id);
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

void BatchRunner::classify_trace(const std::string &trace_key, std::vector<schema::Transaction> txs,
                                 const mch::TieredLookupSource::InterfaceMap &tier1_map,
                                 const ParsedBlockPtr &parsed_block) {
  mch::EmuTraceView view;
  view.trace_id = trace_key;
  view.nodes.push_back(mch::EmuTxRef{.finality = mch::EmuFinality::finalized});
  for (const auto &sb : parsed_block->mc_block_.shard_blocks_) {
    view.shard_states.push_back(sb.block_state);
  }
  view.config = parsed_block->mc_block_.config_;
  auto run = mch::classify(*prep_, trace_key, txs, tier1_map,
                           tier2_enabled_ ? &view.shard_states : nullptr, view.config);
  auto &classified = run.classified;
  classify_stats_ += run.stats;

  ++classification_outcomes_[classified.outcome];
  skipped_payload_actions_ += classified.skipped_payload_actions;
  LOG(DEBUG) << "trace=" << trace_key << " outcome=" << mch::trace_outcome_name(classified.outcome);
  if (mch::is_failure_outcome(classified.outcome)) {
    ++classification_failures_by_category_[classified.failure_category];
    failed_trace_keys_.push_back(trace_key);
    LOG(WARNING) << "trace=" << trace_key << " " << mch::trace_outcome_name(classified.outcome)
                 << " (" << mch::failure_category_name(classified.failure_category)
                 << "): " << classified.failure_reason;
    return;
  }
  if (!classified.failure_reason.empty()) {
    failed_trace_keys_.push_back(trace_key);
    LOG(WARNING) << "trace=" << trace_key << " classified with skipped payload actions: "
                 << classified.failure_reason;
  }

  total_actions_emitted_ += classified.rows.size();
  auto actions_blob = mch::serialize_redis_actions(classified.rows, view);

  std::vector<std::string> tx_hash_b64;
  tx_hash_b64.reserve(txs.size());
  for (const auto &tx : txs) {
    if (tx.in_msg) {
      tx_hash_b64.push_back(td::base64_encode(tx.in_msg->hash.as_slice()));
    }
  }

  td::JsonBuilder jb;
  auto obj = jb.enter_object();
  obj("trace_key", trace_key);
  obj("actions_b64", td::base64_encode(actions_blob));
  obj("tx_hashes", td::json_array([&](auto &arr) {
        for (const auto &hash : tx_hash_b64) {
          arr << hash;
        }
      }));
  obj.leave();
  output_ << jb.string_builder().as_cslice().str() << '\n';
}

void BatchRunner::fail(std::uint32_t seqno, const char *stage, td::Status error) {
  LOG(ERROR) << "Failed during " << stage << " for seqno " << seqno << ": " << error;
  exit_code_->store(7);
  stop();
}

void BatchRunner::finish() {
  output_.flush();
  output_.close();

  const auto prewarm_seqnos = static_cast<std::uint64_t>(seqno_from_) - read_start_;
  const auto range_seqnos = static_cast<std::uint64_t>(seqno_to_) - seqno_from_ + 1;
  const auto pending_at_eof = payloads_by_trace_.size();

  const std::string outcomes = mch::format_counts(classification_outcomes_, mch::trace_outcome_name);
  const std::string failures =
      mch::format_counts(classification_failures_by_category_, mch::failure_category_name);

  std::ostringstream summary;
  summary << "ton-mch-index stats: seqnos read prewarm=" << prewarm_seqnos << " range=" << range_seqnos
          << "; blocks parsed=" << total_blocks_ << "; transactions seen=" << total_transactions_
          << "; traces complete-selected=" << complete_selected_
          << " complete-out-of-range=" << complete_out_of_range_ << " tick-tock-skipped=" << tick_tock_skipped_ << " pending-at-eof=" << pending_at_eof
          << " broken=" << broken_ << "; peak accumulator size=" << peak_accumulator_size_
          << "; outcomes{" << outcomes << "}"
          << " failures{" << failures << "}"
          << "; actions emitted=" << total_actions_emitted_
          << "; skipped payload actions=" << skipped_payload_actions_
          << "; " << classify_stats_.format();
  const double wall = now_s() - run_started_;
  const double other = wall - (t_fetch_ + t_parse_ + t_assemble_ + t_interfaces_ + t_classify_);
  char perf[512];
  std::snprintf(perf, sizeof(perf),
                "ton-mch-index perf: wall=%.1fs fetch=%.1fs parse=%.1fs assemble=%.1fs interfaces=%.1fs "
                "classify=%.1fs other=%.1fs | %.2f seqnos/s, %.1f traces/s, %.2f ms/trace(classify)",
                wall, t_fetch_, t_parse_, t_assemble_, t_interfaces_, t_classify_, other,
                (seqno_to_ - read_start_ + 1) / wall,
                (complete_selected_ + complete_out_of_range_ + tick_tock_skipped_) / wall,
                complete_selected_ ? t_classify_ * 1000.0 / complete_selected_ : 0.0);
  LOG(INFO) << perf;
  std::puts(perf);


  LOG(INFO) << summary.str();
  std::cout << summary.str() << std::endl;
  for (const auto &trace_key : failed_trace_keys_) {
    LOG(INFO) << "Failed trace key: " << trace_key;
    std::cout << "Failed trace key: " << trace_key << std::endl;
  }
  exit_code_->store(0);
  stop();
}
