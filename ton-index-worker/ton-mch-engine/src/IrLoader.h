// MCH IR artifact loader (docs/mch-ir-spec.md §3, §5; docs/mch-engine-v2.md §3.1).
//
// Parses the `0.1-poc` artifact JSON into immutable CompiledMatcher /
// CompiledNode tables, mirroring the Python reference (mch_ir/loader.py +
// engine.py build_matchers / IrMatcher.__init__). Everything is copied out of
// the JSON tree, so the decoded document need not outlive the load.
//
// Match-phase runnability (this stage builds a MATCH walker only, no build
// programs): a matcher is `unrunnable` iff its match phase would consult a host
// PREDICATE the C++ side cannot supply — i.e. a `pred`/`mixed`-with-`where`
// anchor, or a reachable `pred` node / `where` (named predicate) node. Inline
// `where_expr` trees ARE runnable: the loader re-encodes each subtree into a
// self-owned WhereExpr the walker evaluates (interpreter or generated code).
// Builders / shapers / lookups / host-fns / message-types are BUILD-phase
// bindings and do NOT affect matching, so they never make a matcher unrunnable
// here; their names are only collected for the load report. The walker skips
// unrunnable matchers. The Python twin computes the identical skip set from the
// same artifact, so the skip set is itself A/B-checked.
//
// Interning decision: btype comparisons use the raw std::string (btypes are
// short; the walk is match-only over a fixed corpus). A separate interned-id
// table was deliberately deferred — it is a pure perf optimization with no
// observable effect and would require touching the Block substrate. Opcodes are
// normalized to unsigned 32-bit at load.
#pragma once

#include "td/utils/JsonBuilder.h"
#include "td/utils/Status.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mch {

enum class NodeKind { Contract, BlockType, Pred, Any, Or, Recursive };
enum class RecStrategy { Frontier, Cyclic };
enum class AnchorKind { OpcodeSet, BType, Pred, Mixed };

// A node's parsed inline `where (expr)` tree, self-owned: `expr` slices point
// into `buf`, so the pair lives behind one shared_ptr and never moves.
struct WhereExpr {
  std::string buf;     // re-encoded JSON of the where_expr subtree
  td::JsonValue expr;  // decoded view into buf
};

struct CompiledNode {
  NodeKind kind{NodeKind::Any};
  int global_id{-1};  // artifact-global node index (stable id for the codegen table)
  std::uint32_t opcode{0};   // contract (unsigned 32)
  bool has_opcode{false};
  std::string btype;         // block_type
  std::string pred_name;     // pred kind (unrunnable if present)
  std::string where_name;    // named `where` predicate (unrunnable if present)
  bool has_where_expr{false};
  std::shared_ptr<WhereExpr> where_expr;  // set iff has_where_expr
  int slot{-1};
  bool optional{false};
  int child{-1};
  std::vector<int> children;
  int parent{-1};
  std::vector<int> branches;
  int step{-1};
  int exit{-1};
  bool exclusive{false};
  RecStrategy strategy{RecStrategy::Frontier};
};

struct CompiledMatcher {
  std::string name;
  int artifact_index{-1};  // index in the artifact `matchers` array (gen_builds key)
  bool has_build_program{false};
  std::vector<std::string> produces;

  AnchorKind anchor_kind{AnchorKind::OpcodeSet};
  std::unordered_set<std::uint32_t> anchor_opcodes;
  std::unordered_set<std::string> anchor_btypes;

  std::vector<CompiledNode> nodes;      // local pool, remapped, root == 0
  std::vector<std::string> slot_names;  // slot id -> capture name (decl order)
  std::vector<std::string> cards;       // slot id -> "one"|"opt"|"many"

  // Recursion capture ownership (per recursive node id -> owned slot ids).
  std::unordered_map<int, std::set<int>> step_slots;
  std::unordered_map<int, std::set<int>> exit_slots;
  std::set<int> owned_slots;

  bool include_excess{true};
  bool include_bounces{true};
  int priority{100};

  bool unrunnable{false};
  std::string skip_reason;  // e.g. "pred:no_internal_transfer,pton_self_transfer"

  // Host bindings referenced (report only).
  std::set<std::string> ref_preds, ref_builders, ref_shapers;
  std::set<std::string> ref_msgtypes, ref_lookups, ref_fns;
};

struct LoadedIr {
  std::vector<CompiledMatcher> matchers;  // priority order (stable by priority)

  // SHA-256 (hex) of the artifact file bytes — compared against the generated
  // tables' source shas (gen_builds_source_sha / gen_wheres_source_sha).
  std::string source_sha256;

  // Anchor index (built per §3.1; the walk gates on test_self so this is an
  // available optimization, reported but not the gating mechanism).
  std::unordered_map<std::uint32_t, std::vector<int>> op_index;
  std::unordered_map<std::string, std::vector<int>> btype_index;

  int n_nodes{0};       // total artifact nodes
  int n_unrunnable{0};
};

// Load + compile an artifact from a JSON file path.
td::Result<LoadedIr> load_ir(const std::string &path);

}  // namespace mch
