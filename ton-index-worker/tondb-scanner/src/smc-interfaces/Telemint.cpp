#include "Telemint.h"
#include "convert-utils.h"
#include "execute-smc.h"
#include "tokens-tlb.h"
#include "td/utils/utf8.h"


TelemintContract::TelemintContract(block::StdAddress address,
                                   td::Ref<vm::Cell> code_cell,
                                   td::Ref<vm::Cell> data_cell,
                                   AllShardStates shard_states,
                                   std::shared_ptr<block::ConfigInfo> config,
                                   td::Promise<Result> promise) :
  address_(std::move(address)), code_cell_(std::move(code_cell)), data_cell_(std::move(data_cell)),
  shard_states_(std::move(shard_states)), config_(std::move(config)), promise_(std::move(promise))
{
}

td::Result<TelemintContract::Result> TelemintContract::detect(
    const block::StdAddress& address, const td::Ref<vm::Cell>& code_cell,
    const td::Ref<vm::Cell>& data_cell, const std::shared_ptr<block::ConfigInfo>& config)
{
  if (code_cell.is_null() || data_cell.is_null()) {
    return td::Status::Error("Code or data null");
  }

  Result data;
  data.address = address;
  // 1. Get token name
  auto token_name_r = execute_smc_method<1>(address, code_cell, data_cell, config, "get_telemint_token_name", {},
            {vm::StackEntry::Type::t_slice});
  if (token_name_r.is_error()) {
    // LOG(INFO) << "Unable to get token";
    return token_name_r.move_as_error_prefix("get_telemint_token_name failed: ");
  }
  auto token_name_stack = token_name_r.move_as_ok();
  auto token_name_slice = token_name_stack[0].as_slice();
  auto cell_slice = token_name_slice.get();
  tlb::SnakeString str;
  vm::CellSlice clone = cell_slice->clone();
  auto token_name_result = str.load_snake_string(clone);
  if (token_name_result.is_error()) {
    return token_name_result.move_as_error_prefix("load_snake_string failed: ");
  }
  auto token_name = token_name_result.move_as_ok();
  if (!td::check_utf8(token_name)) {
    return td::Status::Error("get_telemint_token_name returned invalid UTF-8");
  }
  data.token_name = std::move(token_name);

  // 2. Get auction state (may throw if no auction)
  auto auction_state_r = execute_smc_method<5>(address, code_cell, data_cell, config, "get_telemint_auction_state", {},
            {vm::StackEntry::Type::t_slice, vm::StackEntry::Type::t_int, vm::StackEntry::Type::t_int,
             vm::StackEntry::Type::t_int, vm::StackEntry::Type::t_int});

  bool has_auction = auction_state_r.is_ok();

  if (has_auction) {
    auto auction_state_stack = auction_state_r.move_as_ok();

    // Parse bidder_address (can be null)
    auto bidder_slice = auction_state_stack[0].as_slice();
    if (bidder_slice->size() > 0) {
      auto bidder_address = convert::to_std_address(bidder_slice);
      if (bidder_address.is_error()) {
        return bidder_address.move_as_error_prefix("bidder address parsing failed: ");
      }
      data.bidder_address = bidder_address.move_as_ok();
    }

    data.bid = auction_state_stack[1].as_int();
    data.bid_ts = auction_state_stack[2].as_int()->to_long();
    data.min_bid = auction_state_stack[3].as_int();
    data.end_time = auction_state_stack[4].as_int()->to_long();
  } else {
    // No auction - set defaults
    data.bidder_address = std::nullopt;
    data.bid = td::RefInt256{true, 0};
    data.bid_ts = 0;
    data.min_bid = td::RefInt256{true, 0};
    data.end_time = 0;
  }

  // 3. Get auction config (returns nulls if no auction, doesn't throw)
  auto auction_config_r = execute_smc_method_nullable<6>(address, code_cell, data_cell, config, "get_telemint_auction_config", {},
            {nullable(vm::StackEntry::Type::t_slice), nullable(vm::StackEntry::Type::t_int),
             nullable(vm::StackEntry::Type::t_int), nullable(vm::StackEntry::Type::t_int),
             nullable(vm::StackEntry::Type::t_int), nullable(vm::StackEntry::Type::t_int)});

  if (auction_config_r.is_error()) {
    return auction_config_r.move_as_error_prefix("get_telemint_auction_config failed: ");
  }

  auto auction_config_stack = auction_config_r.move_as_ok();

  if (auction_config_stack[0].type() != vm::StackEntry::Type::t_null) {
    // Parse beneficiary_address (can be null)
    auto beneficiary_slice = auction_config_stack[0].as_slice();
    if (beneficiary_slice->size() > 0) {
      auto beneficiary_address = convert::to_std_address(beneficiary_slice);
      if (beneficiary_address.is_error()) {
        return beneficiary_address.move_as_error_prefix("beneficiary address parsing failed: ");
      }
      data.beneficiary_address = beneficiary_address.move_as_ok();
    }

    data.initial_min_bid = auction_config_stack[1].as_int();
    data.max_bid = auction_config_stack[2].as_int();
    data.min_bid_step = auction_config_stack[3].as_int();
    data.min_extend_time = auction_config_stack[4].as_int()->to_long();
    data.duration = auction_config_stack[5].as_int()->to_long();
  }

  // 4. Get royalty params
  auto royalty_r = execute_smc_method<3>(address, code_cell, data_cell, config, "royalty_params", {},
            {vm::StackEntry::Type::t_int, vm::StackEntry::Type::t_int, vm::StackEntry::Type::t_slice});

  if (royalty_r.is_error()) {
    return royalty_r.move_as_error_prefix("royalty_params failed: ");
  }

  auto royalty_stack = royalty_r.move_as_ok();

  data.royalty_numerator = royalty_stack[0].as_int()->to_long();
  data.royalty_denominator = royalty_stack[1].as_int()->to_long();

  auto royalty_dest = convert::to_std_address(royalty_stack[2].as_slice());
  if (royalty_dest.is_error()) {
    return royalty_dest.move_as_error_prefix("royalty destination address parsing failed: ");
  }
  data.royalty_destination = royalty_dest.move_as_ok();

  return data;
}

void TelemintContract::start_up()
{
  promise_.set_result(detect(address_, code_cell_, data_cell_, config_));
  stop();
}
