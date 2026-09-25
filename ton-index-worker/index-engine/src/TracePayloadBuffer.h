#pragma once

#include "ClassifyCore.h"
#include "TieredLookupSource.h"
#include "ReplayRun.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace mch {

struct ClassifyInput {
  schema::Trace trace;
  std::vector<schema::Transaction> txs;
  TieredLookupSource::InterfaceMap interfaces;
  std::optional<TraceOutcome> skipped;
};

// Buffers transactions and interface data until a trace becomes terminal, then
// emits one ClassifyInput. Eviction and snapshot seeds leave sticky skip outcomes.
class TracePayloadBuffer {
 public:
  explicit TracePayloadBuffer(const ClassifyPolicy &policy) : policy_(policy) {}

  // Feed assembled blocks in masterchain order; drain terminal traces.
  std::vector<ClassifyInput> add_block(std::uint32_t mc_seqno, const ParsedBlock &block);

  struct Counters {
    std::size_t entries, buffered_txs, buffered_memo_accounts, peak_entries;
    std::uint64_t admitted, capped, evicted, broken, incomplete, missing_root;
  };
  const Counters &counters() const { return counters_; }

  // Insert or overwrite a terminal marker for a trace whose payload is gone
  // (snapshot cap seed, eviction). A sticky entry buffers nothing.
  void seed(const td::Bits256 &trace_id, TraceOutcome outcome);
  std::size_t retain_pending(const std::vector<td::Bits256> &sorted_ids);

  // Drop payloads of the largest non-sticky entries until at least `target_txs`
  // buffered transactions are freed. Each evicted entry becomes a sticky
  // `skipped_evicted`. Returns the number of entries evicted.
  std::size_t evict(std::size_t target_txs);

 private:
  struct Entry {
    std::vector<schema::Transaction> txs;
    TieredLookupSource::InterfaceMap memo;
    std::optional<TraceOutcome> sticky;
  };

  ClassifyPolicy policy_;
  std::unordered_map<td::Bits256, Entry> entries_;
  Counters counters_{};
};

}  // namespace mch
