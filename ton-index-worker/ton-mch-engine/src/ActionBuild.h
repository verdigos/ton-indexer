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
      nft_listing_data, dex_deposit_liquidity_data, dex_withdraw_liquidity_data, staking_data,
      evaa_withdraw_data, vesting_send_message_data, layerzero_packet_data, jvault_stake_data,
      jvault_claim_data, extra;
};

// _base_block_to_action + block_to_action dispatch, over a produced block.
// Returns false for a btype outside the ported fill set (skip-table entry).
bool build_action(Block *b, Action &a);

}  // namespace mch
