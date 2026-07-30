// The embedding entry point: prepare the trace-independent classify state once,
// shared (const) by every classifier spawn.
//
// There is nothing to load — the matcher table is compiled into the binary
// (GenMatchers.h), so this is the whole startup cost of the engine: an advisory
// registry check plus prepare_classify().
//
// This is all that remains of the Phase-G MchClassifierActor header — the
// actor itself was shadow-only ("writes NOTHING") and is deleted per
// python-parity-audit §5.5; its observation job belongs to a separate replay
// tool (audit §4 G4). The emulator's EmuClassifierActor and ton-trace-emulator
// are the live consumers.
#pragma once

#include "ClassifyCore.h"
#include "IrTables.h"

#include "td/utils/Status.h"

#include <memory>
#include <vector>

namespace mch {

// Trace-independent engine state: the compiled matcher table + its setup (skip
// table, generated build-fn map, referenced lookup kinds). `matchers` points at
// the process-lifetime generated table, and `setup.included` indexes into it.
struct MchEnginePrep {
  const std::vector<CompiledMatcher> *matchers = nullptr;
  ClassifySetup setup;
};

// Prepare the classify setup over the compiled-in table. Fails (Result error) on
// a registry conflict or a table/fn mismatch — both are build bugs, and the
// caller decides whether that is fatal (prod-design §4: startup-fatal in
// production). No I/O at all.
td::Result<std::shared_ptr<const MchEnginePrep>> make_engine_prep();

}  // namespace mch
