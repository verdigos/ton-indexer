// EM1.5 shadow classifier — the emulator-header-free value types
// (em1-design §15). EmuFinality MIRRORS the emulator's FinalityState so that the
// converter, the classifier actor and the capture format never include
// TraceEmulator.h; only the bridge TU (EmuShadow.cpp) knows the emulator types
// and translates.
#pragma once

#include "ActionBuild.h"             // Action (built here, serialized before it leaves)
#include "ClassifyDriver.h"          // LookupWants (the tier-2 fetch contract)
#include "EmuActionPayload.h"        // EmuActionPayload (what the result carries, §4.1)
#include "MchClassifierActor.h"      // MchEnginePrep
#include "ParsedBlockLookupSource.h" // InterfaceMap, LookupStats

#include "crypto/block/block.h"  // block::StdAddress
#include "vm/cells/Cell.h"

#include "td/actor/PromiseFuture.h"
#include "td/utils/port/Clocks.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mch {

// Constant global_version for the off-pipeline message parse. It selects
// extra_flags vs legacy ihr_fee in int_msg_info; neither field is read by
// SchemaTraceLoader, so the choice cannot affect classification.
// TODO(EM2): wire live config global_version
inline constexpr int kEmuGlobalVersion = 12;

// Failure/timeout contract (em1-design §16). The cap is a valve, not a working
// limit: expected in-flight under live load is ~60-250.
inline constexpr std::size_t kMaxInFlightClassify = 1024;
inline constexpr std::int64_t kClassifyDeadlineUs = 200'000;
inline constexpr std::int64_t kStallFailOpenUs = 5'000'000;

// §5 admission WORK cap, on node count. The deadline bounds queue WAIT only —
// it is evaluated before work, never during it — so once a trace is admitted
// nothing can shed it, and the measured spread is 130x (13.3 ms tail against a
// ~100 us average) because the cost tracks trace SIZE, not a constant.
//
// 64 is evidence, not a guess: over 5,833 captured emissions the purely
// finalized population — the one the emulator's own txs_count_limit_ does NOT
// gate, since increase_tx_count has a single caller inside emulation
// (TraceEmulator.cpp:78) and block-sourced transactions never touch it — peaked
// at 34 nodes, so 64 sits ~1.9x above the observed max and rejected 0 of 5,833.
// The emulated population is gated at 256 elsewhere and peaked at 57.
//
// It stays a FLAG (--mch-work-cap) because that corpus under-samples exactly the
// traffic carrying the tail: 21 mixed traces, none of them at the ceiling. If
// shed_oversize fires at all in normal traffic, the cap is wrong, not the
// traffic.
inline constexpr std::size_t kMaxClassifyNodes = 64;

// --- tier-2 suspension (em2-tier2-design §5.5-§5.7) --------------------------
//
// A SEPARATE cap from the 1024 admission valve (§5.7): `in_flight` now counts
// requests that may be waiting on IO, and a suspended trace additionally
// retains its converted schema::Transaction vector (message bodies as base64
// BOC) plus the cell anchor for the whole round trip — a materially larger
// per-item footprint than a queued-but-not-started request. Over the cap the
// classifier commits the current pass WITHOUT fetching rather than suspending.
inline constexpr std::size_t kMaxSuspended = 256;

// The suspended-trace sweep (§5.5). Without it, a suspended trace whose
// response never lands wedges the pipeline PERMANENTLY: its insert promise
// never resolves, in_progress_cnt_ never releases, and
// finalized_blocks_inflight_ stays pinned against kMaxFinalizedBlocksInFlight.
// Independent of the 10 s stats line, so the alarm re-arms on the shorter of
// the two.
inline constexpr double kSweepIntervalSec = 1.0;
// Grace past the deadline before a suspended trace is committed all-null. It
// must exceed one fetch round's worst case — §6.5's chained jvault round is up
// to three MGETs, i.e. connect + 3 x socket timeout = 600 ms at the §5.3
// defaults — so the sweep only ever fires on a response that is genuinely LOST,
// never on one that is merely slow. A swept trace is therefore pinned for at
// most deadline + grace + one sweep interval, instead of forever.
inline constexpr std::int64_t kSweepGraceUs = 1'000'000;

// The sweep and the fail-open watchdog have to stay in this order: a suspended
// trace must be released before the emulator concludes the classifier has
// wedged. Break it and a dead store stops degrading classification and starts
// DISABLING it, which is a much larger behaviour change than it looks.
static_assert(kClassifyDeadlineUs + kSweepGraceUs +
                      static_cast<std::int64_t>(kSweepIntervalSec * 1e6) < kStallFailOpenUs,
              "a swept trace must answer well before the stall watchdog fires");

// Tier-2 circuit breaker (§5.6). The existing fail-open watchdog cannot catch a
// dead store on its own — it needs the population to reach kMaxInFlightClassify,
// which the block emulators' own budgets (~280 traces in flight) do not
// guarantee. While the breaker is open every trace degrades to today's
// tier-2-less behaviour at ZERO latency: no suspension, no socket.
inline constexpr std::size_t kBreakerFailures = 5;
inline constexpr std::int64_t kBreakerOpenUs = 30'000'000;

// Monotonic microseconds: the deadline is stamped on an emulator thread and
// read on the classifier's, so it must not move with the wall clock.
inline std::int64_t emu_now_us() {
  return static_cast<std::int64_t>(td::Clocks::monotonic() * 1e6);
}

enum class EmuFinality : std::uint8_t { emulated = 0, confirmed = 1, finalized = 2 };

// Keeps the block's cell store alive for the transaction cells taken off it
// (em1-design §12): those are INTERIOR cells of a lazy boc whose loader holds
// the store only weakly, while the block emulator that owns it stops as soon as
// every insert promise has resolved. Null for listener-emulated traces, which
// reference no block boc.
using EmuCellAnchor = std::shared_ptr<const std::vector<td::Ref<vm::Cell>>>;

struct EmuTxRef {
  block::StdAddress address;
  // LIVE cell, kept loadable by EmuTraceView::anchor. Null == the node had no
  // transaction cell.
  td::Ref<vm::Cell> tx_root;
  std::uint32_t mc_seqno{0};
  EmuFinality finality{EmuFinality::emulated};
};

// A VIEW over one trace emission plus the lifetime handle that makes it
// readable off the emulator's thread. Building it loads no cell, so it cannot
// fail and cannot throw.
struct EmuTraceView {
  std::string trace_id;  // base64(ext_in_msg_hash_norm) == the Redis trace key
  bool tx_limit_exceeded{false};
  // PRE-ORDER FLATTEN: to_tree relinks by message hash, so the tree shape is
  // not needed downstream.
  std::vector<EmuTxRef> nodes;
  ParsedBlockLookupSource::InterfaceMap interfaces;  // already V2-adapted
  EmuCellAnchor anchor;
  std::int64_t sent_us{0}, deadline_us{0};  // emu_now_us basis
};

// The emission's finality: `min` over the node finalities, which is both the
// §2.3 `finality` wire key and the §4.3a guard value — derived in ONE place so
// the payload the insert guards on and the payload it ships cannot disagree.
//
// Lives here rather than next to the decorator so it is reachable without
// TraceEmulator.h, which is what lets --actions-msgpack-test gate it.
inline EmuFinality view_finality(const EmuTraceView &view) {
  if (view.nodes.empty()) {
    // No emission to describe. `emulated` is the least advanced value, so an
    // empty view loses every guard comparison rather than blocking a real one.
    return EmuFinality::emulated;
  }
  EmuFinality f = EmuFinality::finalized;
  for (const EmuTxRef &node : view.nodes) {
    f = std::min(f, node.finality);
  }
  return f;
}

enum class EmuClassifyOutcome : std::uint8_t {
  classified,
  classify_failed,
  convert_failed,   // the classifier ran
  shed_admission,
  shed_deadline,
  shed_oversize,     // §5 work cap: refused before it took a queue slot
  bypassed_disabled  // it did not (§16)
};

struct EmuClassifyResult {
  std::string trace_id;
  EmuClassifyOutcome outcome{EmuClassifyOutcome::bypassed_disabled};
  // W3 (§3): BYTES, not rows. The Action vector and the EmuCellAnchor that used
  // to ride along here are both gone — serialization now happens inside
  // EmuClassifierActor::classify, while the view (and therefore the anchor) is
  // still alive, so nothing anchor-backed survives into the insert. That
  // deletes a whole use-after-free class rather than documenting it: the old
  // declaration-order rule only protected this struct's own destruction, and a
  // caller that moved `actions` out was back to a dangling interior cell.
  // TraceInserter now needs no mch value type at all — just these plain strings.
  EmuActionPayload payload;
  bool used_fallback{false};       // rows came from ClassifyResult::fallback_actions
  std::size_t unported_btypes{0};  // produced blocks build_action() declined
  bool failure{false};
  std::string failure_reason;
  FailureCategory failure_category{FailureCategory::none};
  ParsedBlockLookupSource::LookupStats lookup_stats;
  // serialize_us is SEPARATE from classify_us on purpose (§3): both land in the
  // same critical section, and folding them together would hide which half a
  // regression came from. classify_us already includes write_capture(), so
  // neither is comparable against a capture-enabled run.
  std::int64_t queue_us{0}, classify_us{0}, serialize_us{0};
};

// Shared gate state: written by the decorator on the emulator's threads and by
// the classifier on its own.
struct EmuGate {
  // Set once at construction (--mch-classify-deadline-ms), before any emulator
  // thread runs, and only read afterwards — hence plain, not atomic.
  std::int64_t classify_deadline_us{kClassifyDeadlineUs};
  std::atomic<std::size_t> in_flight{0};
  // Seeded at construction, NOT left at 0: the watchdog measures staleness from
  // this stamp, and a zero start would make "no response yet" indistinguishable
  // from "process just started" — a classifier that wedges on its very first
  // request would then never trip the fail-open.
  std::atomic<std::int64_t> last_response_us{emu_now_us()};
  std::atomic<bool> disabled{false};  // sticky fail-open, §16.3
};

// The ASYNCHRONOUS batched tier-2 fetch (§5.1). The classifier suspends across
// this call — that is the whole point, a network round must not become a
// head-of-line stall — so it cannot be the driver's synchronous Tier2Fetch.
//
// CONTRACT: answers EXACTLY ONCE, with either a value (an entry per requested
// key, a miss as an explicit null) or an ERROR, which the classifier treats as
// an all-null answer AND as evidence for the circuit breaker (§8.1: an error
// and an all-null response are the same classification, byte for byte). A
// promise dropped without an answer is answered with an error by
// ~LambdaPromise, so lookup-actor death resumes the trace too.
using EmuTier2Fetch =
    std::function<void(LookupWants, td::Promise<std::map<std::string, Value>>)>;

struct EmuShadowConfig {
  std::shared_ptr<const MchEnginePrep> prep;  // nullptr = feature off
  std::string capture_dir;                    // empty = no capture
  int global_version{kEmuGlobalVersion};
  // Unset = tier 2 is OFF: the collect pass records the keys nobody will fetch,
  // round 0 commits, and the emission is byte-identical to EM1.5. W5 wires this
  // to the EmuLookupActor pool behind --mch-kvrocks (lookup_pool_fetch()).
  EmuTier2Fetch tier2_fetch;
  // Insert immediately and use the response for stats only
  // (--mch-classify-nonblocking): EM1 behavior, minus the serialization copy.
  bool nonblocking{false};
  // §5 work cap (--mch-work-cap). Lives here rather than as another
  // TraceScheduler ctor parameter so the decorator reads it from the same object
  // that already carries `gate` and `nonblocking`.
  std::size_t max_nodes{kMaxClassifyNodes};
  // REQUIRED whenever `prep` is set: both the decorator and the actor
  // dereference it unguarded.
  std::shared_ptr<EmuGate> gate;
};

}  // namespace mch
