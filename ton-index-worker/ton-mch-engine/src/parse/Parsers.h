// Internal registry surface: the per-family message-body parse fns, one TU per
// protocol family (ParseJettons.cpp, ParseDns.cpp, ...). Declared here so
// MsgParse.cpp's message_parsers() map can reference them across TU boundaries.
// NOT part of the public MsgParse.h surface.
#pragma once

#include "../Value.h"

#include "td/utils/Status.h"
#include "vm/cells/Cell.h"

namespace mch {

// Jetton family (ParseJettons.cpp).
td::Result<Value> parse_jetton_transfer(const td::Ref<vm::Cell> &body);
td::Result<Value> parse_jetton_internal_transfer(const td::Ref<vm::Cell> &body);
td::Result<Value> parse_jetton_burn(const td::Ref<vm::Cell> &body);
td::Result<Value> parse_jetton_notify(const td::Ref<vm::Cell> &body);
td::Result<Value> parse_jetton_mint(const td::Ref<vm::Cell> &body);
td::Result<Value> parse_minter_jetton_mint(const td::Ref<vm::Cell> &body);
td::Result<Value> parse_never(const td::Ref<vm::Cell> &body);

// DNS family (ParseDns.cpp).
td::Result<Value> parse_change_dns(const td::Ref<vm::Cell> &body);

// Vesting family (ParseVesting.cpp).
td::Result<Value> parse_vesting_send_message(const td::Ref<vm::Cell> &body);

// pTON family (ParsePton.cpp).
td::Result<Value> parse_pton_transfer(const td::Ref<vm::Cell> &body);

// DeDust family (ParseDedust.cpp).
td::Result<Value> parse_dedust_payout_from_pool(const td::Ref<vm::Cell> &body);
td::Result<Value> parse_dedust_swap_notification(const td::Ref<vm::Cell> &body);

// Coffee family (ParseCoffee.cpp).
td::Result<Value> parse_coffee_create_vault(const td::Ref<vm::Cell> &body);

// EVAA withdraw family (ParseEvaa.cpp).
td::Result<Value> parse_evaa_withdraw_master(const td::Ref<vm::Cell> &body);
td::Result<Value> parse_evaa_withdraw_collateralized(const td::Ref<vm::Cell> &body);
td::Result<Value> parse_evaa_withdraw_fail_excess(const td::Ref<vm::Cell> &body);

// LayerZero family (ParseLayerZero.cpp).
td::Result<Value> parse_layerzero_oapp_execute_callback(const td::Ref<vm::Cell> &body);

// JVault family (ParseJvault.cpp).
td::Result<Value> parse_jvault_unstake_jettons(const td::Ref<vm::Cell> &body);
td::Result<Value> parse_jvault_unstake_request(const td::Ref<vm::Cell> &body);

}  // namespace mch
