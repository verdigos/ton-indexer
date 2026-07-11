// Production classification actor (Phase G step 3 slice 3, prod-design §1).
//
// A THIN td::actor over the already-gated library pieces: it takes an assembled
// ParsedBlock (post interfaces stage), groups its transactions per trace, runs
// each trace through SchemaTraceLoader -> ClassifyCore::classify_trace with a
// ParsedBlockLookupSource, and LOGS a per-trace summary. It writes NOTHING —
// no Action rows, no DB, no effect on the pipeline. All classification logic
// lives in the lib TUs the offline gates already prove; this actor only wires
// them to production ParsedBlock data (shadow mode).
#pragma once

#include "ClassifyCore.h"
#include "IrLoader.h"
#include "ParsedBlockLookupSource.h"

#include "IndexData.h"  // ParsedBlockPtr (tondb-scanner, PUBLIC include)

#include "td/actor/actor.h"
#include "td/utils/Status.h"

#include <memory>
#include <string>
#include <vector>

namespace mch {

// Trace-independent engine state, loaded ONCE and shared (const) across every
// classifier actor spawn: the IR artifact + its runnable-matcher setup (skip
// table, generated build-fn map, referenced lookup kinds).
struct MchEnginePrep {
  LoadedIr ir;
  ClassifySetup setup;
};

// Load the artifact and prepare the classify setup. Fails (Result error) on a
// bad artifact or a table/fn mismatch — the caller decides whether that is fatal
// (prod-design §4: startup-fatal in production). No I/O beyond reading the file.
td::Result<std::shared_ptr<const MchEnginePrep>> load_engine_prep(const std::string &artifact_path);

// Log-only per-trace outcome (shadow mode).
struct TraceClassifySummary {
  std::string trace_id;
  std::size_t action_count{0};
  std::vector<std::string> matcher_names;
  bool failure{false};
  std::string failure_reason;
  FailureCategory failure_category{FailureCategory::none};
  ParsedBlockLookupSource::LookupStats lookup_stats;  // tier attribution
};

class MchClassifierActor : public td::actor::Actor {
 public:
  MchClassifierActor(std::shared_ptr<const MchEnginePrep> prep, ParsedBlockPtr block,
                     std::uint32_t mc_seqno)
      : prep_(std::move(prep)), block_(std::move(block)), mc_seqno_(mc_seqno) {
  }

  void start_up() override;

 private:
  std::shared_ptr<const MchEnginePrep> prep_;
  ParsedBlockPtr block_;
  std::uint32_t mc_seqno_;
};

}  // namespace mch
