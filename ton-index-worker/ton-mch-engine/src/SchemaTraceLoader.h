// Production trace loader: maps tondb-scanner's
// schema::Transaction / schema::Message (IndexData.h) into the engine's own
// TraceModel structs (Trace / Transaction / Message / MsgContent), so that
// to_tree / init_block / ClassifyCore run UNCHANGED on production data.
//
// KEY: schema::Trace carries only `edges` (no transaction list), and our
// to_tree rebuilds the EventNode tree purely from message-hash linking, so the
// input here is the trace's already-gathered schema::Transactions (the caller /
// actor collects them, possibly across mc-seqnos), NOT schema::Trace, and the
// edges are not needed.
#pragma once

#include "ActionBuild.h"
#include "TraceModel.h"

#include "IndexData.h"  // schema::Transaction (tondb-scanner, PUBLIC include)

#include "td/utils/Status.h"

#include <string>
#include <vector>

namespace mch {

// Hash, canonical account and aborted status for a schema transaction.
void fill_transaction_fields(Transaction &tx, const schema::Transaction &st);

// Rescue row with all sorted distinct hashes, without constructing a Trace.
Action create_unknown_action(const std::string &trace_id, const std::vector<schema::Transaction> &txs);

// Map a trace's schema::Transactions -> the engine Trace (transactions only;
// `interfaces` is the LookupSource's concern, filled separately). Field map is
// documented per-field in the .cpp. Never throws, returns a Trace.
td::Result<Trace> schema_to_trace(const std::string &trace_id,
                                  const std::vector<schema::Transaction> &txs);

}  // namespace mch
