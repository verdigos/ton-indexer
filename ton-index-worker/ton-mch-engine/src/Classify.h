// End-to-end classify loop (`--classify`, Phase B3).
//
// Ports the Python outer loop VERBATIM (event_processing.process_event_async +
// mch_ir/engine.py IrMatcher.try_build):
//   root = Block('root'); root.connect(init_block(tree))
//   for m in matchers (priority order):
//     for b in root.bfs_iter():          # FIFO; b's next_blocks are read
//       if b.parent is None:             # AFTER try_build(b) ran (generator
//         try_build(m, b)                # resume order) — port exactly
// Loop consequences (all inherited from Python, not invented here):
//  - a block produced by matcher m is NOT visited by m's own ongoing BFS (it
//    hangs off an ancestor whose next_blocks were already read) but IS visible
//    to every later matcher's fresh BFS — cross-matcher production (e.g.
//    ethena anchoring on a produced jetton_transfer) works;
//  - consumed blocks keep their next_blocks, so the old topology is still
//    traversed; the `parent is None` guard only filters try_build calls.
//
// try_build = match (root absorb=False) -> capture slots -> two-phase lookups
// (FixtureLookupSource) -> GENERATED build fn -> on rejection: nothing (clean
// §10.7); on accept: produced Block from the arena (btype = outcome or
// produces[0], data/failed/broken from the outcome), merge_blocks(consumed)
// (false => clean rejection), THEN the root-level excess/bounce auto-append
// list from the anchor's next_blocks (canonical order, include_* flags) —
// engine.py try_build lines 443-451 exactly. Flows are recorded on the dump
// (value_flow application is Phase E).
//
// DUMP FORMAT CONTRACT (Python twin: scratchpad/classify/py_classify.py):
//   SKIP <matcher> reason=...   # once, sorted (same rules as BuildSmoke.h)
//   === <fixture-basename>
//   ACTION <matcher> anchor=<key> btype=<bt> failed=0|1 broken=0|1
//          data={..} consumed=[<key>,..] auto=[<key>,..]
// consumed = SORTED keys of the produced block's children_blocks (== the merge
// set); auto = canonical (min_lt) order, unsorted. Value rendering per
// BuildSmoke.h. Rejections print nothing (matches Python r in (None, [])).
//
// gen_builds() must have been generated from THIS artifact (sha-checked).
#pragma once

#include "BlockTree.h"  // TraceContext (loader callback return type)

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mch {

struct CoreAction;  // ClassifyCore.h
class LookupSource;  // BuildRuntime.h

// How run_dump acquires a trace's TraceContext for a fixture path. Path A = the
// msgpack loader (load_trace_context); the schema A/B gate injects a callback
// that synthesizes schema::Transactions and runs SchemaTraceLoader (SchemaGate.cpp).
using TraceContextLoader = std::function<td::Result<TraceContext>(const std::string &)>;

// How run_dump obtains the LookupSource for a trace. Empty (default) => the
// fixture-backed FixtureLookupSource over ctx.trace.interfaces; the production-
// path gate (--classify-prod / --actions-prod) injects a ParsedBlockLookupSource
// factory over a synthesized account_interfaces_ map (SchemaGate.cpp).
using LookupSourceFactory = std::function<std::shared_ptr<LookupSource>(TraceContext &)>;

int run_classify(const std::string &artifact_path, const std::vector<std::string> &paths);

// Shared classify/actions dump over an injectable context loader (the render +
// engine loop live here; only ctx acquisition varies A vs B). Exported so the
// schema A/B gate reuses the exact rendering, keeping path B byte-comparable.
int run_dump(const std::string &artifact_path, const std::vector<std::string> &paths,
             bool actions_mode, const TraceContextLoader &load_ctx,
             const LookupSourceFactory &lookup_factory = {});

// Phase E part 2 (`--actions`): serialized-Action-row dump. Same collect->pass
// pipeline as run_classify; each surviving post-pass produced block is rendered
// as an Action row (block_tree_serializer.py _fill_* port, scoped to the ~18
// composite btypes that fire in the corpus). Python twin:
// scratchpad/classify/py_actions.py. Dump line:
//   ACTION anchor=<key> {id=..,type=..,<directional>,<composite maps>..}
// Value rendering per BuildDriver render_value (sorted keys). See Classify.cpp
// header block for the staged-out set (accounts/roles/initiating-tx extension).
int run_actions(const std::string &artifact_path, const std::vector<std::string> &paths);

}  // namespace mch
