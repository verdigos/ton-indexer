// W1 shadow classifier — the emulator-header-free value types (em1-design §15).
// EmuFinality MIRRORS the emulator's FinalityState so that the converter and
// the classifier actor never include TraceEmulator.h; only the bridge TU
// (EmuShadow.cpp) knows the emulator types and translates.
#pragma once

#include "ActionBuild.h"             // Action (the rows the classifier hands back)
#include "MchClassifierActor.h"      // MchEnginePrep
#include "ParsedBlockLookupSource.h" // InterfaceMap, LookupStats

#include "crypto/block/block.h"  // block::StdAddress
#include "vm/cells/Cell.h"

#include "td/utils/port/Clocks.h"

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
  std::int64_t sent_us{0}, deadline_us{0};  // emu_now_us basis
};

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
  // HANDED BACK: a VType::Cell value inside a returned Action can be an
  // anchor-backed interior cell (custom_payload / forward_payload fills reach
  // StaticBocExtCells whose bag only this anchor keeps alive), so the anchor
  // must outlive the rows, not the request. DECLARED BEFORE `actions` on
  // purpose: members destruct in reverse declaration order, so the anchor
  // outlives the rows that point into it. That only covers this struct's own
  // destruction — a write-back must carry the anchor ALONGSIDE the rows into
  // the insert; a caller that moves `actions` out and drops the result is back
  // to a use-after-free, which the ordering alone cannot prevent.
  EmuCellAnchor anchor;
  std::vector<Action> actions;
  bool used_fallback{false};       // rows came from ClassifyResult::fallback_actions
  std::size_t unported_btypes{0};  // produced blocks build_action() declined
  bool failure{false};
  std::string failure_reason;
  FailureCategory failure_category{FailureCategory::none};
  ParsedBlockLookupSource::LookupStats lookup_stats;
  std::int64_t queue_us{0}, classify_us{0};
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
};

}  // namespace mch
