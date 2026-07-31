// Action-row construction (block_tree_serializer.py port), lib side.
//
// Split out of the harness Classify.cpp (engine M1): the Action struct and
// build_action/fill_* are pipeline logic that the production actor needs, while
// the dump-side render_action + stdout printing stay with the harness. No
// behavior change — the moved code is byte-identical, proven by the twin dumps.
#pragma once

#include "BlockTree.h"
#include "Value.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mch {

// The rendered Action (fixed schema; unset fields are Null, rendered "null").
struct Action {
  std::string action_id;
  std::string type;
  bool success{true};
  std::int64_t start_lt{0}, end_lt{0}, start_utime{0}, end_utime{0}, mc_seqno_end{0};
  std::vector<std::string> tx_hashes;
  std::vector<std::string> accounts;  // Step 2a: role-less action_accounts set
  Value source, source_secondary, destination, destination_secondary;
  Value asset, asset_secondary, asset2, asset2_secondary;
  Value amount, value, opcode;
  Value ton_transfer_data, jetton_transfer_data, jetton_swap_data, nft_transfer_data,
      nft_listing_data, nft_mint_data, dex_deposit_liquidity_data, dex_withdraw_liquidity_data,
      staking_data, evaa_supply_data, evaa_withdraw_data, vesting_send_message_data,
      layerzero_packet_data,
      jvault_stake_data, jvault_claim_data, change_dns_record_data,
      coffee_create_pool_data, coffee_staking_deposit_data, coffee_staking_withdraw_data,
      extra;
  // Child-action recursion (serialize_blocks, block_tree_serializer.py:1668-1682).
  // Empty on a top-level row; on a child row, the action_id of the block that
  // consumed it and the btypes of every ancestor up the consumption chain.
  std::string parent_action_id;
  std::vector<std::string> ancestor_type;
};

// One row serialize_blocks emits: the block plus the two fields that are NOT a
// property of the block alone but of WHERE it sits in the consumption tree.
// Lives here rather than in ClassifyCore.h so ActionBuild owns the whole Action
// vocabulary (and the core can build a row without depending on rendering).
struct ActionRow {
  Block *block{nullptr};
  std::string parent_action_id;
  std::vector<std::string> ancestor_type;
};

// _calc_action_id (block_tree_serializer.py:110-120): base64(sha256(the lowest-lt
// event node's msg_hash|tx_hash + btype)). A pure function of the block, which is
// what lets the serializer link a child to its parent before either is built.
std::string calc_action_id(const Block *b);

// _base_block_to_action + block_to_action dispatch, over a produced block.
// Returns false for a btype outside the ported fill set (skip-table entry).
bool build_action(Block *b, Action &a);

// The same, over a serialize_blocks row: fills parent_action_id/ancestor_type too.
bool build_action(const ActionRow &row, Action &a);

// create_unknown_action (block_tree_serializer.py:1683-1706): the synthetic row
// a trace gets when classification SUCCEEDS but produces nothing. Emitted by
// the adapter when ClassifyResult::unknown_trace is set.
//
// Carries the ONLY action_id in the system not derived from a hash: Python uses
// the trace id (`:1690`), which on the emulated path is also the Redis trace
// key (em2-writeback-design.md §10). The lt/utime/mc_seqno_end range comes off
// the Trace, so every loader owes those fields a value — see TraceLoader.h.
Action create_unknown_action(const Trace &trace);

}  // namespace mch
