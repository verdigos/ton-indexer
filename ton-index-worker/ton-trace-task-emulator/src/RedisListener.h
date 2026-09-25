#pragma once
#include <td/actor/actor.h>
#include <validator/impl/external-message.hpp>
#include <emulator/transaction-emulator.h>
#include <sw/redis++/redis++.h>
#include <msgpack.hpp>
#include "IndexData.h"
#include "TraceEmulator.h"
#include "TraceInterfaceDetector.h"
#include "TonConnectProcessor.h"
#include "emu/EmuClassifierActor.h"

struct TraceEmulationResult {
  std::string task_id;
  td::Result<Trace> trace;
  std::optional<ton::BlockId> mc_block_id;
  // Serialized actions; empty when classification was not requested.
  std::optional<td::Result<std::string>> actions;
};

class RedisListener : public td::actor::Actor {
private:
  sw::redis::Redis redis_;
  std::string queue_name_;
  td::actor::ActorId<DbScanner> db_scanner_;
  std::function<void(TraceEmulationResult, td::Promise<td::Unit>)> trace_processor_;
  
  schema::MasterchainBlockDataState mc_data_state_;
  ton::BlockId current_mc_block_id_;

  mch::EmuClassifierConfig classifier_config_;
  std::vector<td::actor::ActorOwn<mch::EmuClassifierActor>> classifiers_;
  std::size_t next_classifier_{0};

public:
  RedisListener(std::string redis_dsn, std::string queue_name, td::actor::ActorId<DbScanner> db_scanner, std::function<void(TraceEmulationResult, td::Promise<td::Unit>)> trace_processor,
                mch::EmuClassifierConfig classifier_config)
      : redis_(sw::redis::Redis(redis_dsn)), queue_name_(queue_name), db_scanner_(db_scanner), trace_processor_(std::move(trace_processor)),
        classifier_config_(std::move(classifier_config)) {};

  virtual void start_up() override;

  void alarm() override;
  void set_mc_data_state(schema::MasterchainBlockDataState mc_data_state);

private:
  void trace_error(std::string task_id, std::optional<ton::BlockId> mc_block_id, td::Status error);
  void trace_received(TraceTask task, schema::MasterchainBlockDataState mc_data_state, Trace trace);
  void trace_interfaces_error(std::string task_id, ton::BlockId mc_block_id, td::Status error);
  void finish_processing(std::string task_id, ton::BlockId mc_block_id, Trace trace, bool with_actions);
  void classification_finished(std::string task_id, ton::BlockId mc_block_id, Trace trace, td::Result<mch::EmuClassifyResult> R);
  void insert_result(std::string task_id, ton::BlockId mc_block_id, Trace trace, std::optional<td::Result<std::string>> actions);
  
  // Process regular trace task
  void process_trace_task(TraceTask task);
  
  // Process TonConnect task (converts to regular trace task)
  void process_tonconnect_task(TonConnectTraceTask task);
};
