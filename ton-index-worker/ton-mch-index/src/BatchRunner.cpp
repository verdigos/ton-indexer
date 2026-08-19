#include "BatchRunner.h"

#include "td/utils/Time.h"

#include <cstdio>

#include "BlockInterfacesDetector.h"
#include "emu/EmuActionSerialize.h"
#include "emu/EmuCelldbLookup.h"
#include "emu/EmuTypes.h"

#include "vm/excno.hpp"  // vm::VmError is not a std::exception.

#include "td/utils/JsonBuilder.h"
#include "td/utils/base64.h"
#include "td/utils/logging.h"

#include <algorithm>
#include <exception>
#include <iostream>
#include <sstream>
#include <utility>

namespace {

const char *category_name(mch::FailureCategory category) {
  switch (category) {
    case mch::FailureCategory::none:
      return "none";
    case mch::FailureCategory::lookup_infra_fail:
      return "lookup_infra_fail";
    case mch::FailureCategory::engine_fault:
      return "engine_fault";
    case mch::FailureCategory::malformed_trace:
      return "malformed_trace";
  }
  return "?";
}

}  // namespace

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

  // Keep the latest detected state for every account seen across the full read window.
  for (auto &kv : parsed_block->account_interfaces_) {
    account_interfaces_[kv.first] = std::move(kv.second);
  }

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
      auto it = transactions_by_trace_.find(trace.trace_id);
      std::vector<schema::Transaction> txs;
      if (it != transactions_by_trace_.end()) {
        txs = std::move(it->second);
        transactions_by_trace_.erase(it);
      }
      if (trace.mc_seqno_end >= static_cast<std::int32_t>(seqno_from_) &&
          trace.mc_seqno_end <= static_cast<std::int32_t>(seqno_to_)) {
        if (!trace.external_hash.has_value() && !emit_tick_tock_) {
          // Tick-tock-rooted traces: production stores no actions for this
          // class (policy undecided — python saves only some of them), so the
          // default mirrors prod. --emit-tick-tock restores full emission.
          ++tick_tock_skipped_;
          continue;
        }
        ++complete_selected_;
        const auto output_hash = trace.external_hash_norm.value_or(trace.external_hash.value_or(trace.trace_id));
        const std::string trace_key = td::base64_encode(output_hash.as_slice());
        LOG(INFO) << "Selected complete trace " << trace_key
                  << " at masterchain seqno " << trace.mc_seqno_end;
        std::stable_sort(txs.begin(), txs.end(),
                         [](const auto &left, const auto &right) { return left.lt < right.lt; });
        const double classify_started = now_s();
        classify_trace(trace_key, std::move(txs), parsed_block);
        t_classify_ += now_s() - classify_started;
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

void BatchRunner::classify_trace(const std::string &trace_key, std::vector<schema::Transaction> txs,
                                 const ParsedBlockPtr &parsed_block) {
  AllShardStates shard_states;
  for (const auto &sb : parsed_block->mc_block_.shard_blocks_) {
    shard_states.push_back(sb.block_state);
  }
  mch::EmuCelldbTier2 tier2(&shard_states, parsed_block->mc_block_.config_);
  mch::ParsedBlockLookupSource::Tier2Hook hook;
  if (tier2_enabled_) {
    hook = [&tier2](const std::string &kind, const std::vector<mch::Value> &args) {
      return tier2.fetch(kind, args);
    };
  }
  mch::ParsedBlockLookupSource src(&account_interfaces_, std::move(hook));
  std::vector<mch::Action> rows;
  std::vector<std::string> matcher_names;
  std::size_t scrubbed = 0;
  bool unknown_row = false;
  mch::SchemaClassifyResult classified;
  bool threw = false;
  try {
    classified = mch::classify_schema_trace(*prep_, trace_key, txs, src, rows, matcher_names, scrubbed,
                                            unknown_row);
  } catch (const vm::VmError &e) {
    threw = true;
    classified.failure = true;
    classified.failure_reason = std::string("vm exception: ") + e.get_msg();
    classified.failure_category = mch::FailureCategory::engine_fault;
  } catch (const vm::VmNoGas &e) {
    threw = true;
    classified.failure = true;
    classified.failure_reason = std::string("vm exception: ") + e.get_msg();
    classified.failure_category = mch::FailureCategory::engine_fault;
  } catch (const vm::VmVirtError &e) {
    threw = true;
    classified.failure = true;
    classified.failure_reason = std::string("vm exception: ") + e.get_msg();
    classified.failure_category = mch::FailureCategory::engine_fault;
  } catch (const std::exception &e) {
    threw = true;
    classified.failure = true;
    classified.failure_reason = std::string("exception: ") + e.what();
    classified.failure_category = mch::FailureCategory::engine_fault;
  } catch (...) {
    threw = true;
    classified.failure = true;
    classified.failure_reason = "unknown exception";
    classified.failure_category = mch::FailureCategory::engine_fault;
  }

  const auto &stats = src.stats();
  lookup_stats_.tier1_hits += stats.tier1_hits;
  lookup_stats_.tier2_hits += stats.tier2_hits;
  for (const auto &[kind, count] : stats.tier2_hits_by_kind) {
    lookup_stats_.tier2_hits_by_kind[kind] += count;
  }
  lookup_stats_.misses += stats.misses;
  for (const auto &[kind, count] : stats.misses_by_kind) {
    lookup_stats_.misses_by_kind[kind] += count;
  }

  if (threw || classified.failure) {
    ++classified_failed_;
    ++classification_failures_by_category_[classified.failure_category];
    failed_trace_keys_.push_back(trace_key);
    LOG(WARNING) << "trace=" << trace_key << " FAILED (" << category_name(classified.failure_category)
                 << "): " << classified.failure_reason;
    return;
  }

  ++classified_ok_;
  total_actions_emitted_ += rows.size();

  mch::EmuTraceView view;
  view.trace_id = trace_key;
  view.nodes.push_back(mch::EmuTxRef{.finality = mch::EmuFinality::finalized});
  mch::ActionSerializeStats ser_stats;
  const std::string actions_blob = mch::serialize_actions(rows, view, &ser_stats);

  std::vector<std::string> tx_hash_b64;
  tx_hash_b64.reserve(txs.size());
  for (const auto &tx : txs) {
    if (tx.in_msg) {
      // Production joins traces to the plain inbound message hash.
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
  const auto pending_at_eof = transactions_by_trace_.size();

  std::string tier2_hits_by_kind;
  for (const auto &[kind, count] : lookup_stats_.tier2_hits_by_kind) {
    if (!tier2_hits_by_kind.empty()) {
      tier2_hits_by_kind += ",";
    }
    tier2_hits_by_kind += kind + "=" + std::to_string(count);
  }

  std::string misses_by_kind;
  for (const auto &[kind, count] : lookup_stats_.misses_by_kind) {
    if (!misses_by_kind.empty()) {
      misses_by_kind += ",";
    }
    misses_by_kind += kind + "=" + std::to_string(count);
  }

  auto failures = [this](mch::FailureCategory category) {
    auto it = classification_failures_by_category_.find(category);
    return it == classification_failures_by_category_.end() ? 0 : it->second;
  };

  std::ostringstream summary;
  summary << "ton-mch-index stats: seqnos read prewarm=" << prewarm_seqnos << " range=" << range_seqnos
          << "; blocks parsed=" << total_blocks_ << "; transactions seen=" << total_transactions_
          << "; traces complete-selected=" << complete_selected_
          << " complete-out-of-range=" << complete_out_of_range_ << " tick-tock-skipped=" << tick_tock_skipped_ << " pending-at-eof=" << pending_at_eof
          << " broken=" << broken_ << "; peak accumulator size=" << peak_accumulator_size_
          << "; classified ok=" << classified_ok_ << " failed=" << classified_failed_
          << " failures{none=" << failures(mch::FailureCategory::none)
          << ",lookup_infra_fail=" << failures(mch::FailureCategory::lookup_infra_fail)
          << ",engine_fault=" << failures(mch::FailureCategory::engine_fault)
          << ",malformed_trace=" << failures(mch::FailureCategory::malformed_trace) << "}"
          << "; actions emitted=" << total_actions_emitted_
          << "; account interfaces=" << account_interfaces_.size()
          << "; lookups{tier1_hits=" << lookup_stats_.tier1_hits
          << ",tier2_hits=" << lookup_stats_.tier2_hits << ",misses=" << lookup_stats_.misses
          << ",misses_by_kind={" << misses_by_kind << "}}"
          << ";tier2_by_kind={" << tier2_hits_by_kind << "}";
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
