// W1 shadow classifier — the emulator-header-free value types (em1-design §15).
// EmuFinality MIRRORS the emulator's FinalityState so that the converter and
// the classifier actor never include TraceEmulator.h; only the bridge TU
// (EmuShadow.cpp) knows the emulator types and translates.
#pragma once

#include "ActionBuild.h"             // Action (the rows the classifier hands back)
#include "EmuActionPayload.h"        // EmuActionPayload (the write-back bytes)
#include "EmuCelldbLookup.h"         // Tier2Budget, Tier2Stats, AllShardStates
#include "MchClassifierActor.h"      // MchEnginePrep
#include "ParsedBlockLookupSource.h" // InterfaceMap, LookupStats

#include "crypto/block/block.h"  // block::StdAddress
#include "vm/cells/Cell.h"

#include "td/utils/port/Clocks.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mch {

// Constant global_version for the off-pipeline message parse. It selects
// extra_flags vs legacy ihr_fee in int_msg_info; neither field is read by
// SchemaTraceLoader, so the choice cannot affect classification.
// TODO: wire live config global_version
inline constexpr int kEmuGlobalVersion = 12;

// Failure/timeout contract (em1-design §16). The cap is a valve, not a working
// limit: expected in-flight under live load is ~60-250.
inline constexpr std::size_t kMaxInFlightClassify = 1024;
inline constexpr std::int64_t kClassifyDeadlineUs = 200'000;
inline constexpr std::int64_t kStallFailOpenUs = 5'000'000;

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
  // W3 tier-2 inputs: the shard states this trace was emulated against and
  // their config. Cheap shared handles (a vector of cell Refs, a shared_ptr) —
  // copying them costs refcounts, not cells. EMPTY for a listener-emulated
  // trace, which is exactly the "no tier 2" case the hook degrades to.
  AllShardStates shard_states;
  std::shared_ptr<block::ConfigInfo> config;
  std::int64_t sent_us{0}, deadline_us{0};  // emu_now_us basis
};

// The emission's finality: Python's min(tx.finality) over the trace. ONE
// derivation, because the wire `finality` key and the insert's staleness guard
// must agree — two copies would let the value a trace is guarded on drift from
// the one its own payload advertises.
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
  bypassed_disabled  // it did not (§16)
};

struct EmuClassifyResult {
  std::string trace_id;
  EmuClassifyOutcome outcome{EmuClassifyOutcome::bypassed_disabled};
  // BYTES, not rows. The Action vector and the EmuCellAnchor that used to ride
  // along here are both gone — serialization now happens inside
  // EmuClassifierActor::classify, while the view (and therefore the anchor) is
  // still alive, so nothing anchor-backed survives into the insert. That
  // deletes a whole use-after-free class rather than documenting it: a
  // declaration-order rule only protects this struct's own destruction, and a
  // caller that moved `actions` out was back to a dangling interior cell.
  // TraceInserter needs no mch value type at all — just these plain strings.
  EmuActionPayload payload;
  bool used_fallback{false};       // rows came from ClassifyResult::fallback_actions
  std::size_t unported_btypes{0};  // produced blocks build_action() declined
  bool failure{false};
  std::string failure_reason;
  FailureCategory failure_category{FailureCategory::none};
  ParsedBlockLookupSource::LookupStats lookup_stats;
  Tier2Stats tier2_stats;
  // serialize_us is SEPARATE from classify_us on purpose: both land in the same
  // critical section, and folding them together would hide which half a
  // regression came from.
  std::int64_t queue_us{0}, classify_us{0}, serialize_us{0};
};

// Shared gate state: written by the decorator on the emulator's threads and by
// the classifier on its own.
struct EmuGate {
  std::atomic<std::size_t> in_flight{0};
  // Seeded at construction, NOT left at 0: the watchdog measures staleness from
  // this stamp, and a zero start would make "no response yet" indistinguishable
  // from "process just started" — a classifier that wedges on its very first
  // request would then never trip the fail-open.
  std::atomic<std::int64_t> last_response_us{emu_now_us()};
  std::atomic<bool> disabled{false};  // sticky fail-open, §16.3
};

struct EmuShadowConfig {
  std::shared_ptr<const MchEnginePrep> prep;  // nullptr = feature off
  int global_version{kEmuGlobalVersion};
  // REQUIRED whenever `prep` is set: both the decorator and the actor
  // dereference it unguarded.
  std::shared_ptr<EmuGate> gate;
  // W3: celldb tier-2 lookups. On by default wherever the classifier runs;
  // off degrades to the tier-1-only behaviour that shipped in W1/W2.
  bool tier2{true};
  Tier2Budget tier2_budget;
};

}  // namespace mch
