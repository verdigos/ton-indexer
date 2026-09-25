// Action-row construction shared by classification and output adapters.
#pragma once

#include "BlockTree.h"
#include "Value.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mch {

// Row columns shared by the struct and output adapters.
#define MCH_ACTION_COLUMNS(X) \
  X(source) \
  X(source_secondary) \
  X(destination) \
  X(destination_secondary) \
  X(asset) \
  X(asset_secondary) \
  X(asset2) \
  X(asset2_secondary) \
  X(amount) \
  X(value) \
  X(opcode)

struct Action {
  std::string action_id;
  std::string type;
  bool success{true};
  std::int64_t start_lt{0}, end_lt{0}, start_utime{0}, end_utime{0}, mc_seqno_end{0};
  std::vector<std::string> tx_hashes;
  std::vector<std::string> accounts;  // role-less action_accounts set
  std::map<std::uint32_t, std::string> value_extra_currencies;
#define MCH_DECL(name) Value name;
  MCH_ACTION_COLUMNS(MCH_DECL)
#undef MCH_DECL
  Value data;
  // Empty on a top-level row; on a child row, the action_id of the block that
  // consumed it and the btypes of every ancestor up the consumption chain.
  std::string parent_action_id;
  std::vector<std::string> ancestor_type;
  // Set after the final data mutation in classification; empty for ad hoc actions.
  std::string encoded_payload;
};

// One serialized row: the block, its precomputed id, and the fields determined
// by where it sits in the consumption tree.
struct ActionRow {
  Block *block{nullptr};
  std::string action_id;
  std::string parent_action_id;
  std::vector<std::string> ancestor_type;
  // action_id of the gasless_request marker this row is an immediate result
  // of (empty otherwise); rendered as data.parent_gasless_action.
  std::string parent_gasless_action;
};

// The block's lowest-lt event node (nullptr for an empty block).
const EventNode *root_event_node(const Block *b);

// base64(sha256(root_event_node's msg_hash|tx_hash + btype)).
std::string calc_action_id(const Block *b);

// Dispatch over a produced block. Returns false for a btype outside the fill
// set (skip-table entry).
bool build_action(Block *b, Action &a);

bool build_action(const ActionRow &row, Action &a);

bool omitted(const Value &value);

// Synthetic row a trace gets when classification succeeds but produces
// nothing. Emitted when ClassifyResult::unknown_trace is set.
//
// Carries the only action_id in the system not derived from a hash: the
// trace id, which on the emulated path is also the Redis trace key. The
// lt/utime/mc_seqno_end range comes from the Trace, so every loader must
// populate those fields; see TraceModel.h.
// Also used as the writer rescue; keeps all sorted distinct transaction hashes.
Action create_unknown_action(const Trace &trace);

}  // namespace mch
