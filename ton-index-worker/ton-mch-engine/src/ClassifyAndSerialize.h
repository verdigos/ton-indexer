// Shared schema classification and Redis action serialization for batch and
// emulator callers.
#pragma once

#include "ClassifyCore.h"
#include "RedisActionSerialize.h"
#include "emu/EmuTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mch {

struct ClassifyAndSerializeOptions {
  bool tier2_enabled{true};
  // Emulator interface maps are incremental, so their missing entries are not
  // authoritative. Batch completion maps opt in to authoritative negatives.
  bool authoritative_tier1{false};
  // The emulator writes fallback rows; the batch CLI omits failed traces.
  bool serialize_failure_rows{true};
  // Tests and alternate runtimes may inject a tier-2 source. Product callers
  // leave this empty and use CelldbTier2Lookup.
  TieredLookupSource::Tier2Hook tier2_hook{};
};

struct ClassifyAndSerializeResult {
  SchemaClassifyResult classified;
  std::string actions_blob;
  TieredLookupSource::LookupStats lookup_stats;
  Tier2Stats tier2_stats;
  RedisActionSerializeStats serialize_stats;
  std::int64_t serialize_us{0};
};

ClassifyAndSerializeResult classify_and_serialize(
    const MchEnginePrep &prep, const std::vector<schema::Transaction> &txs,
    const EmuTraceView &view, const TieredLookupSource::InterfaceMap &interfaces,
    ClassifyAndSerializeOptions options = {});

}  // namespace mch
