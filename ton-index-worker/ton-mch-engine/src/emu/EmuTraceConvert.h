// EM1 converter (em1-design §2): raw emulator transaction cells -> the
// schema::Transaction shape SchemaTraceLoader is gated against, via the
// ParseQuery static seam (DataParser.h) — the SAME parsers the indexing
// pipeline runs, so there is no second parser to keep true.
#pragma once

#include "EmuTypes.h"

#include "IndexData.h"  // schema::Transaction

#include "td/utils/Status.h"

#include <vector>

namespace mch {

// Per node: deserialize tx_boc, ParseQuery::parse_transaction(tx_root,
// address.workchain, gv), then mc_seqno from the wrapper node. ANY node failing
// => whole-trace error: a partial tree would classify wrong, never silently.
td::Result<std::vector<schema::Transaction>> emu_to_schema_txs(const EmuTraceSnapshot &snap,
                                                               int global_version);

}  // namespace mch
