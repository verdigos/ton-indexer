// The embedding entry point: load an IR artifact once and prepare the
// trace-independent classify state shared (const) by every classifier spawn.
//
// This is all that remains of the Phase-G MchClassifierActor header — the
// actor itself was shadow-only ("writes NOTHING") and is deleted per
// python-parity-audit §5.5; its observation job belongs to a separate replay
// tool (audit §4 G4). The emulator's EmuClassifierActor and ton-trace-emulator
// are the live consumers.
#pragma once

#include "ClassifyCore.h"
#include "IrLoader.h"

#include "td/utils/Status.h"

#include <memory>
#include <string>

namespace mch {

// Trace-independent engine state: the IR artifact + its runnable-matcher setup
// (skip table, generated build-fn map, referenced lookup kinds).
struct MchEnginePrep {
  LoadedIr ir;
  ClassifySetup setup;
};

// Load the artifact and prepare the classify setup. Fails (Result error) on a
// bad artifact or a table/fn mismatch — the caller decides whether that is fatal
// (prod-design §4: startup-fatal in production). No I/O beyond reading the file.
td::Result<std::shared_ptr<const MchEnginePrep>> load_engine_prep(const std::string &artifact_path);

}  // namespace mch
