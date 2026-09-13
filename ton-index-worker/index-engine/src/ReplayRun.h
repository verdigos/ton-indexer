// Parameters of one classification replay run and the policy constants it
// obeys. A run drives the scanner pipeline over an inclusive seqno window,
// classifies every trace that completes inside [classify_from, classify_to]
// and hands rows to the sink chosen by `mode`. Live mode inserts; task mode
// replaces rows per completion seqno. One controller and one assembler per
// run; an aborted run is destroyed, never resumed.
//
// Root policy is fixed, not configured: any root transaction is admitted
// (system accounts included, as python does); a trace that is a single
// tick-tock transaction takes `TraceOutcome::skipped_lone_tick_tock`.
#pragma once

#include "crypto/common/bitstring.h"  // td::Bits256

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mch {

// Tunables with their defaults. Every value is a configuration constant, not a
// measurement; change here or through the CLI, never per trace.
struct ClassifyPolicy {
  // Per-trace transaction cap; exceeding it makes the trace `capped` (python's
  // `big_traces_threshold`).
  std::size_t trace_cap = 4000;
  // Classification workers; the block barrier waits for all jobs of a block.
  std::uint32_t workers = 4;
  // Per-job wall-clock warning threshold; slow jobs retain their outcome and rows.
  // Wall-clock cutoffs would make replay nondeterministic.
  double job_timeout_s = 5.0;
  // Process RSS ceiling sampled once per block; exceeding it fails the run.
  std::uint64_t rss_budget_bytes = std::uint64_t{16} << 30;
  // Task mode: most seqnos claimed by one worker in one claim transaction.
  std::uint32_t run_max_seqnos = 256;
  // Task mode: refuse completion seqnos above `C_live - live_margin`.
  std::uint32_t live_margin = 256;
};

enum class RunMode { live, task };

// Trace metadata from PostgreSQL used by task mode to plan the window and to
// validate the assembled result against what live indexing recorded.
struct TraceInventory {
  td::Bits256 trace_id;
  std::uint32_t mc_seqno_start;
  std::uint32_t mc_seqno_end;
  std::uint32_t nodes;
};

struct ReplayRun {
  RunMode mode = RunMode::live;
  // Blocks read by the scanner pipeline. `scan_to` is unbounded in live mode.
  std::uint32_t scan_from = 0;
  std::uint32_t scan_to = std::numeric_limits<std::uint32_t>::max();
  // Traces completing outside [classify_from, classify_to] are drained silently.
  std::uint32_t classify_from = 0;
  std::uint32_t classify_to = std::numeric_limits<std::uint32_t>::max();
  // Live mode: seqnos below this are processed without any insert (replay).
  // `min(kvrocks_frontier, pg_progress) + 1` at start-up.
  std::uint32_t no_insert_below = 0;
  // Task mode trace task: classify only this trace id; empty = every trace.
  std::optional<td::Bits256> classify_filter;
  // Task mode: the traces whose rows this run replaces (seqno task: every
  // trace completing in the window; trace task: one entry).
  std::vector<TraceInventory> expected;
  // RocksDB secondary instance directory (task mode).
  std::string state_dir;
  ClassifyPolicy policy;
};

struct SnapshotInfo {
  std::uint32_t seqno;
  std::uint32_t oldest_healthy;
};

// The snapshot precedes the insert range, and its next block is still retained.
bool snapshot_usable(const SnapshotInfo &snapshot, std::uint32_t next_seqno,
                     std::uint32_t oldest_available);

ReplayRun live_bounds(std::uint32_t next_seqno, std::uint32_t pg_next_seqno,
                      std::uint32_t oldest_available, std::optional<SnapshotInfo> snapshot);

struct TaskPlanInput {
  std::uint32_t from, to;
  std::vector<TraceInventory> inventory;
  std::uint32_t oldest_available;
  std::pair<std::uint32_t, std::uint32_t> partitioned;
  std::uint32_t live_frontier;
  std::optional<td::Bits256> trace_filter;
};

enum class TaskPlanRefusal { none, no_partition, inside_live_window, history_unavailable };

struct TaskPlan {
  ReplayRun run;
  TaskPlanRefusal refusal;
};

TaskPlan plan_task_run(const TaskPlanInput &in, const ClassifyPolicy &policy);

}  // namespace mch
