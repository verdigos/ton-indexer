// EM1 capture (em1-design §7): one JSON file per classified emission, dumping
// the CONVERTER INPUTS (raw tx BOCs + adapted interfaces), not the classify
// outputs — so a later replay re-runs the exact same emu_to_schema_txs path.
// JSON via td::JsonBuilder: no new dependency.
#pragma once

#include "EmuTypes.h"

#include "td/utils/Status.h"

#include <string>

namespace mch {

// Writes <dir>/<trace_id_urlsafe>.<root finality>.json. The directory must
// exist (the actor creates it once at start-up).
td::Status write_capture(const std::string &dir, const EmuTraceSnapshot &snap, int global_version);

}  // namespace mch
