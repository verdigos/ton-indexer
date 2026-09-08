#pragma once
#include <optional>
#include <utility>
#include "td/actor/actor.h"
#include "queue_state.h"
#include "IndexData.h"


enum ErrorCode {
  DB_ERROR = 500,
  CACHED_CODE_HASH_NO_ENTITY = 501,
  DATA_PARSING_ERROR = 502,
  GET_METHOD_WRONG_RESULT = 503,
  ADDITIONAL_CHECKS_FAILED = 504,
  EVENT_PARSING_ERROR = 505,

  CODE_HASH_NOT_FOUND = 600,
  ENTITY_NOT_FOUND = 601
};


class InsertManagerInterface: public td::actor::Actor {
public:
  struct InsertResult {
    std::optional<bool> is_leader;
  };
  struct ResumeState {
    std::uint32_t next_seqno;
    bool initialized_from_cli;
    std::uint32_t pg_next_seqno;
    std::optional<std::uint32_t> kvrocks_next_seqno;
  };

  struct TraceInventoryRow {
    td::Bits256 trace_id;
    std::uint32_t mc_seqno_start, mc_seqno_end, nodes;
  };

  virtual void get_trace_inventory(std::uint32_t from_seqno, std::uint32_t to_seqno,
                                   td::Promise<std::vector<TraceInventoryRow>> promise) = 0;
  virtual void get_partitioned_seqno_bounds(
      td::Promise<std::pair<std::uint32_t, std::uint32_t>> promise) = 0;
  virtual void insert(std::uint32_t mc_seqno, ParsedBlockPtr block_ds, bool force, td::Promise<QueueState> queued_promise,
                      td::Promise<InsertResult> inserted_promise) = 0;
  virtual void get_insert_queue_state(td::Promise<QueueState> promise) = 0;
  virtual void get_existing_seqnos(td::Promise<std::vector<std::uint32_t>> promise, std::int32_t from_seqno = 0, std::int32_t to_seqno = 0) = 0;
  virtual void ensure_resume_state_initialized(td::Promise<bool> promise, std::int32_t from_seqno = 0) = 0;
  virtual void get_resume_seqno(td::Promise<ResumeState> promise, std::int32_t from_seqno = 0, std::int32_t to_seqno = 0) = 0;
};
