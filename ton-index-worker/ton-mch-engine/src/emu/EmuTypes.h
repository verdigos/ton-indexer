// EM1 shadow classifier — the emulator-header-free value types (em1-design §3).
//
// EmuFinality MIRRORS the emulator's FinalityState so that the converter, the
// classifier actor and the capture format never include TraceEmulator.h; only
// the bridge TU (EmuShadow.cpp) knows the emulator types and translates.
#pragma once

#include "MchClassifierActor.h"      // MchEnginePrep
#include "ParsedBlockLookupSource.h" // InterfaceMap

#include "crypto/block/block.h"  // block::StdAddress

#include "td/utils/buffer.h"  // td::BufferSlice

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
// TODO(EM2): wire live config global_version
inline constexpr int kEmuGlobalVersion = 12;

// Snapshots over kMaxPendingSnapshots in flight are dropped: a shadow feature
// must not grow the emulator's memory without bound.
inline constexpr std::size_t kMaxPendingSnapshots = 1024;

enum class EmuFinality : std::uint8_t { emulated = 0, confirmed = 1, finalized = 2 };

struct EmuTxRec {
  block::StdAddress address;
  // SERIALIZED AT SNAPSHOT TIME, not a Ref: an emulator tx cell can be lazily
  // backed by the block's cell store, which is freed once the sink returns,
  // while the shadow classifier reads it asynchronously afterwards. Empty ==
  // the node had no transaction cell. Deserialized in EmuTraceConvert.
  td::BufferSlice tx_boc;
  std::uint32_t mc_seqno{0};
  EmuFinality finality{EmuFinality::emulated};
};

struct EmuTraceSnapshot {
  std::string trace_id;  // base64(ext_in_msg_hash_norm) == the Redis trace key
  bool tx_limit_exceeded{false};
  // PRE-ORDER FLATTEN: to_tree relinks by message hash, so the tree shape is
  // not needed downstream.
  std::vector<EmuTxRec> nodes;
  ParsedBlockLookupSource::InterfaceMap interfaces;  // already V2-adapted
};

struct EmuShadowConfig {
  std::shared_ptr<const MchEnginePrep> prep;  // nullptr = feature off
  std::string capture_dir;                    // empty = no capture
  int global_version{kEmuGlobalVersion};
  // In-flight snapshot counter. REQUIRED whenever `prep` is set: both the
  // decorator and the actor dereference it unguarded.
  std::shared_ptr<std::atomic<std::size_t>> pending;
};

}  // namespace mch
