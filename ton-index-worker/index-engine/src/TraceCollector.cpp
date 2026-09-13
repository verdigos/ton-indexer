#include "TraceCollector.h"

#include <algorithm>
#include <utility>

namespace mch {
namespace {
void free_payload(TraceCollector::Counters &counters, auto &entry) {
  counters.buffered_txs -= entry.txs.size();
  counters.buffered_memo_accounts -= entry.memo.size();
  entry.txs.clear();
  entry.txs.shrink_to_fit();
  ParsedBlockLookupSource::InterfaceMap{}.swap(entry.memo);
}
}  // namespace

std::vector<ClassifyInput> TraceCollector::add_block(std::uint32_t mc_seqno, const ParsedBlock &block) {
  for (const auto &shard_block : block.blocks_) {
    for (const auto &tx : shard_block.transactions) {
      auto &entry = entries_[tx.trace_id];
      counters_.entries = entries_.size();
      counters_.peak_entries = std::max(counters_.peak_entries, counters_.entries);
      if (entry.sticky) {
        continue;
      }
      if (entry.txs.size() >= policy_.trace_cap) {
        entry.sticky = TraceOutcome::skipped_capped;
        ++counters_.capped;
        free_payload(counters_, entry);
        continue;
      }
      entry.txs.push_back(tx);
      ++counters_.buffered_txs;
      auto detected = block.account_interfaces_.find(tx.account);
      if (detected == block.account_interfaces_.end()) {
        continue;
      }
      auto [slot, inserted] = entry.memo.try_emplace(tx.account);
      if (inserted) {
        ++counters_.buffered_memo_accounts;
      }
      auto &memo = slot->second;
      for (const auto &iface : detected->second) {
        auto existing = std::find_if(memo.begin(), memo.end(), [&](const auto &record) {
          return record.index() == iface.index();
        });
        if (existing == memo.end()) {
          memo.push_back(iface);
        } else {
          *existing = iface;
        }
      }
    }
  }

  std::vector<ClassifyInput> inputs;
  for (const auto &trace : block.traces_) {
    auto it = entries_.find(trace.trace_id);
    if (trace.state == schema::Trace::State::pending) {
      continue;
    }
    ClassifyInput input{trace, {}, {}, std::nullopt};
    if (trace.state == schema::Trace::State::broken) {
      input.skipped = TraceOutcome::skipped_broken;
      ++counters_.broken;
      if (it != entries_.end()) free_payload(counters_, it->second);
    } else if (it == entries_.end()) {
      input.skipped = TraceOutcome::skipped_missing_root;
      ++counters_.missing_root;
    } else if (it->second.sticky) {
      input.skipped = *it->second.sticky;
    } else if (std::none_of(it->second.txs.begin(), it->second.txs.end(), [&](const auto &tx) {
                 return tx.hash == trace.trace_id;
               })) {
      input.skipped = TraceOutcome::skipped_missing_root;
      ++counters_.missing_root;
      free_payload(counters_, it->second);
    } else if (static_cast<std::int64_t>(it->second.txs.size()) != trace.nodes_) {
      input.skipped = TraceOutcome::skipped_incomplete;
      ++counters_.incomplete;
      free_payload(counters_, it->second);
    } else {
      std::stable_sort(it->second.txs.begin(), it->second.txs.end(),
                       [](const auto &left, const auto &right) { return left.lt < right.lt; });
      counters_.buffered_txs -= it->second.txs.size();
      counters_.buffered_memo_accounts -= it->second.memo.size();
      input.txs = std::move(it->second.txs);
      input.interfaces = std::move(it->second.memo);
      ++counters_.admitted;
    }
    if (it != entries_.end()) {
      entries_.erase(it);
    }
    inputs.push_back(std::move(input));
  }

  counters_.entries = entries_.size();
  return inputs;
}

void TraceCollector::seed(const td::Bits256 &trace_id, TraceOutcome outcome) {
  auto &entry = entries_[trace_id];
  free_payload(counters_, entry);
  entry.sticky = outcome;
  counters_.entries = entries_.size();
  counters_.peak_entries = std::max(counters_.peak_entries, counters_.entries);
}

std::size_t TraceCollector::retain_pending(const std::vector<td::Bits256> &sorted_ids) {
  std::size_t erased = 0;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (!it->second.sticky && !std::binary_search(sorted_ids.begin(), sorted_ids.end(), it->first)) {
      counters_.buffered_txs -= it->second.txs.size();
      counters_.buffered_memo_accounts -= it->second.memo.size();
      it = entries_.erase(it);
      ++erased;
    } else {
      ++it;
    }
  }
  counters_.entries = entries_.size();
  return erased;
}

}  // namespace mch
