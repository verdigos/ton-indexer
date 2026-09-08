#include "TraceCollector.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace mch {

std::vector<ClassifyInput> TraceCollector::add_block(std::uint32_t mc_seqno, const ParsedBlock &block) {
  for (const auto &shard_block : block.blocks_) {
    for (const auto &tx : shard_block.transactions) {
      auto &entry = entries_[tx.trace_id];
      counters_.entries = entries_.size();
      counters_.peak_entries = std::max(counters_.peak_entries, counters_.entries);
      if (!entry.capped) {
        if (entry.txs.size() >= policy_.trace_cap) {
          entry.capped = true;
          ++counters_.capped;
          counters_.buffered_txs -= entry.txs.size();
          entry.txs.clear();
          entry.txs.shrink_to_fit();
        } else {
          entry.txs.push_back(tx);
          ++counters_.buffered_txs;
        }
      }
      auto detected = block.account_interfaces_.find(tx.account);
      if (detected != block.account_interfaces_.end()) {
        auto &memo = entry.memo[tx.account];
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
  }

  std::vector<ClassifyInput> inputs;
  for (const auto &trace : block.traces_) {
    auto it = entries_.find(trace.trace_id);
    if (it != entries_.end() && it->second.root_seqno == UINT32_MAX) {
      it->second.root_seqno = trace.mc_seqno_start;
    }
    if (trace.state == schema::Trace::State::pending) {
      continue;
    }
    if (it != entries_.end()) {
      counters_.buffered_txs -= it->second.txs.size();
    }
    ClassifyInput input{trace, {}, {}, std::nullopt};
    if (trace.state == schema::Trace::State::broken) {
      input.skipped = TraceOutcome::skipped_broken;
      ++counters_.broken;
    } else if (it == entries_.end()) {
      if (std::int64_t(mc_seqno) - trace.mc_seqno_start >= policy_.lifetime_w) {
        input.skipped = TraceOutcome::skipped_expired;
      } else {
        input.skipped = TraceOutcome::skipped_missing_root;
        ++counters_.missing_root;
      }
    } else if (it->second.capped) {
      // Capping frees the root along with the rest of the transactions.
      input.skipped = TraceOutcome::skipped_capped;
    } else if (std::none_of(it->second.txs.begin(), it->second.txs.end(), [&](const auto &tx) {
                 return tx.hash == trace.trace_id;
               })) {
      input.skipped = TraceOutcome::skipped_missing_root;
      ++counters_.missing_root;
    } else if (static_cast<std::int64_t>(it->second.txs.size()) != trace.nodes_) {
      input.skipped = TraceOutcome::skipped_incomplete;
      ++counters_.incomplete;
    } else {
      std::stable_sort(it->second.txs.begin(), it->second.txs.end(),
                       [](const auto &left, const auto &right) { return left.lt < right.lt; });
      input.txs = std::move(it->second.txs);
      input.interfaces = std::move(it->second.memo);
      ++counters_.admitted;
    }
    if (it != entries_.end()) {
      entries_.erase(it);
    }
    inputs.push_back(std::move(input));
  }

  for (auto it = entries_.begin(); it != entries_.end();) {
    auto &entry = it->second;
    assert(entry.root_seqno != UINT32_MAX);
    if (entry.root_seqno == UINT32_MAX) {
      entry.root_seqno = mc_seqno;
    }
    if (std::int64_t(mc_seqno) - entry.root_seqno >= policy_.lifetime_w) {
      counters_.buffered_txs -= entry.txs.size();
      it = entries_.erase(it);
      ++counters_.expired;
    } else {
      ++it;
    }
  }
  counters_.entries = entries_.size();
  return inputs;
}

}  // namespace mch
