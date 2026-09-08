#pragma once

#include "ClassifyCore.h"
#include "ParsedBlockLookupSource.h"
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
  ParsedBlockLookupSource::InterfaceMap interfaces;
  std::optional<TraceOutcome> skipped;
};

class TraceCollector {
 public:
  explicit TraceCollector(const ClassifyPolicy &policy) : policy_(policy) {}

  // Feed assembled blocks in masterchain order; drain terminal traces before expiry.
  std::vector<ClassifyInput> add_block(std::uint32_t mc_seqno, const ParsedBlock &block);

  struct Counters {
    std::size_t entries, buffered_txs, peak_entries;
    std::uint64_t admitted, capped, expired, broken, incomplete, missing_root;
  };
  const Counters &counters() const { return counters_; }

 private:
  struct Entry {
    std::vector<schema::Transaction> txs;
    ParsedBlockLookupSource::InterfaceMap memo;
    std::uint32_t root_seqno{UINT32_MAX};
    bool capped{false};
  };

  ClassifyPolicy policy_;
  std::unordered_map<td::Bits256, Entry> entries_;
  Counters counters_{};
};

}  // namespace mch
