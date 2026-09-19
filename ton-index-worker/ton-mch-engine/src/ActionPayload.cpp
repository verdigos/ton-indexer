#include "ActionPayload.h"

#include "ActionJson.h"

#include "Value.h"

#include <msgpack.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mch {
namespace {

using Packer = msgpack::packer<msgpack::sbuffer>;

struct GroupField {
  const char *legacy;
  const char *group;
  Value Action::*member;
};

constexpr std::array<GroupField, 40> kGroups{{
    {"ton_transfer_data", "data", &Action::ton_transfer_data},
    {"jetton_transfer_data", "data", &Action::jetton_transfer_data},
    {"jetton_swap_data", "data", &Action::jetton_swap_data},
    {"nft_transfer_data", "data", &Action::nft_transfer_data},
    {"nft_listing_data", "listing", &Action::nft_listing_data},
    {"nft_mint_data", "data", &Action::nft_mint_data},
    {"dex_deposit_liquidity_data", "data", &Action::dex_deposit_liquidity_data},
    {"dex_withdraw_liquidity_data", "data", &Action::dex_withdraw_liquidity_data},
    {"staking_data", "data", &Action::staking_data},
    {"evaa_supply_data", "data", &Action::evaa_supply_data},
    {"evaa_withdraw_data", "data", &Action::evaa_withdraw_data},
    {"evaa_liquidate_data", "data", &Action::evaa_liquidate_data},
    {"vesting_send_message_data", "data", &Action::vesting_send_message_data},
    {"vesting_add_whitelist_data", "data", &Action::vesting_add_whitelist_data},
    {"tonco_deploy_pool_data", "data", &Action::tonco_deploy_pool_data},
    {"multisig_create_order_data", "data", &Action::multisig_create_order_data},
    {"multisig_approve_data", "data", &Action::multisig_approve_data},
    {"multisig_execute_data", "data", &Action::multisig_execute_data},
    {"cocoon_worker_payout_data", "data", &Action::cocoon_worker_payout_data},
    {"cocoon_proxy_payout_data", "data", &Action::cocoon_proxy_payout_data},
    {"cocoon_proxy_charge_data", "data", &Action::cocoon_proxy_charge_data},
    {"cocoon_client_top_up_data", "data", &Action::cocoon_client_top_up_data},
    {"cocoon_register_proxy_data", "data", &Action::cocoon_register_proxy_data},
    {"cocoon_unregister_proxy_data", "data", &Action::cocoon_unregister_proxy_data},
    {"cocoon_client_register_data", "data", &Action::cocoon_client_register_data},
    {"cocoon_client_change_secret_hash_data", "data", &Action::cocoon_client_change_secret_hash_data},
    {"cocoon_client_request_refund_data", "data", &Action::cocoon_client_request_refund_data},
    {"cocoon_grant_refund_data", "data", &Action::cocoon_grant_refund_data},
    {"cocoon_client_increase_stake_data", "data", &Action::cocoon_client_increase_stake_data},
    {"cocoon_client_withdraw_data", "data", &Action::cocoon_client_withdraw_data},
    {"layerzero_packet_data", "packet", &Action::layerzero_packet_data},
    {"layerzero_send_data", "data", &Action::layerzero_send_data},
    {"layerzero_dvn_verify_data", "data", &Action::layerzero_dvn_verify_data},
    {"jvault_stake_data", "data", &Action::jvault_stake_data},
    {"jvault_claim_data", "data", &Action::jvault_claim_data},
    {"change_dns_record_data", "data", &Action::change_dns_record_data},
    {"coffee_create_pool_data", "data", &Action::coffee_create_pool_data},
    {"coffee_staking_deposit_data", "data", &Action::coffee_staking_deposit_data},
    {"coffee_staking_withdraw_data", "data", &Action::coffee_staking_withdraw_data},
    {"extra", "extra", &Action::extra},
}};

struct Rename {
  const char *legacy_group;
  const char *legacy_key;
  const char *key;
};

constexpr std::array<Rename, 19> kRenames{{
    {"ton_transfer_data", "content", "comment"},
    {"ton_transfer_data", "encrypted", "comment_encrypted"},
    {"jetton_transfer_data", "is_encrypted_comment", "comment_encrypted"},
    {"nft_transfer_data", "nft_item_index", "nft_index"},
    {"nft_listing_data", "nft_item_index", "nft_index"},
    {"nft_mint_data", "nft_item_index", "nft_index"},
    {"dex_withdraw_liquidity_data", "asset1_out", "asset1"},
    {"dex_withdraw_liquidity_data", "asset2_out", "asset2"},
    {"dex_withdraw_liquidity_data", "burned_nft_index", "nft_index"},
    {"dex_withdraw_liquidity_data", "burned_nft_address", "nft_address"},
    {"coffee_create_pool_data", "amount_1", "amount1"},
    {"coffee_create_pool_data", "amount_2", "amount2"},
    {"coffee_staking_deposit_data", "minted_item_index", "nft_index"},
    {"coffee_staking_deposit_data", "minted_item_address", "nft_address"},
    {"change_dns_record_data", "value", "record_value"},
    {"tonco_deploy_pool_data", "jetton0_minter", "asset1"},
    {"tonco_deploy_pool_data", "jetton1_minter", "asset2"},
    {"tonco_deploy_pool_data", "jetton0_router_wallet", "dex_jetton_wallet_1"},
    {"tonco_deploy_pool_data", "jetton1_router_wallet", "dex_jetton_wallet_2"},
}};

void pack_string(Packer &packer, std::string_view value) {
  packer.pack_str(static_cast<std::uint32_t>(value.size()));
  packer.pack_str_body(value.data(), static_cast<std::uint32_t>(value.size()));
}

void pack_binary(Packer &packer, const std::string &value) {
  packer.pack_bin(static_cast<std::uint32_t>(value.size()));
  packer.pack_bin_body(value.data(), static_cast<std::uint32_t>(value.size()));
}

const char *canonical_key(const char *legacy_group, const std::string &key) {
  for (const auto &rename : kRenames) {
    if (key == rename.legacy_key && std::strcmp(legacy_group, rename.legacy_group) == 0) {
      return rename.key;
    }
  }
  return key.c_str();
}

void pack_value(Packer &packer, const Value &value);
bool same_value(const Value &lhs, const Value &rhs);

void pack_fields(Packer &packer, const Value::Fields &fields, const char *legacy_group = nullptr) {
  std::vector<std::pair<std::string, const Value *>> canonical;
  canonical.reserve(fields.size());
  for (const auto &[key, value] : fields) {
    const std::string name = legacy_group == nullptr ? key : canonical_key(legacy_group, key);
    auto existing = std::find_if(canonical.begin(), canonical.end(), [&](const auto &entry) {
      return entry.first == name;
    });
    if (existing != canonical.end()) {
      if (!same_value(*existing->second, value)) {
        throw std::runtime_error("conflicting action payload key " + name);
      }
      continue;
    }
    canonical.emplace_back(name, &value);
  }
  packer.pack_map(static_cast<std::uint32_t>(canonical.size()));
  for (const auto &[key, value] : canonical) {
    pack_string(packer, key);
    pack_value(packer, *value);
  }
}

bool same_value(const Value &lhs, const Value &rhs) {
  msgpack::sbuffer lhs_buffer;
  Packer lhs_packer(&lhs_buffer);
  pack_value(lhs_packer, lhs);
  msgpack::sbuffer rhs_buffer;
  Packer rhs_packer(&rhs_buffer);
  pack_value(rhs_packer, rhs);
  return lhs_buffer.size() == rhs_buffer.size() &&
         std::memcmp(lhs_buffer.data(), rhs_buffer.data(), lhs_buffer.size()) == 0;
}

void pack_value(Packer &packer, const Value &value) {
  switch (value.t) {
    case VType::Null:
    case VType::Block:
      packer.pack_nil();
      return;
    case VType::Bool:
      packer.pack(value.boolean);
      return;
    case VType::Int:
    case VType::Amount:
      if (value.num.is_null()) packer.pack_nil();
      else pack_string(packer, value.num->to_dec_string());
      return;
    case VType::Str: {
      std::string text = value.str;
      std::erase(text, '\0');
      pack_string(packer, text);
      return;
    }
    case VType::Account:
      if (value.addr_none) packer.pack_nil();
      else pack_string(packer, canonicalize_or_passthrough(value.str));
      return;
    case VType::Asset:
      if (value.is_ton || !value.has_jetton) packer.pack_nil();
      else pack_string(packer, canonicalize_or_passthrough(value.str));
      return;
    case VType::Bytes:
      pack_binary(packer, value.str);
      return;
    case VType::Cell: {
      auto boc = td_boc_serialize_crc(value.cell);
      if (boc.is_error()) packer.pack_nil();
      else pack_binary(packer, boc.ok());
      return;
    }
    case VType::List:
      packer.pack_array(static_cast<std::uint32_t>(value.items->size()));
      for (const auto &item : *value.items) pack_value(packer, item);
      return;
    case VType::Dict:
    case VType::Obj:
      pack_fields(packer, *value.fields);
      return;
  }
}

const char *group_name(const Action &action, const GroupField &field) {
  if (action.type == "auction_outbid" && std::strcmp(field.legacy, "ton_transfer_data") == 0) {
    return "comment_data";
  }
  return field.group;
}

}  // namespace

std::string encode_action_payload(const Action &action) {
  std::vector<const GroupField *> present;
  std::unordered_set<std::string> groups;
  for (const auto &field : kGroups) {
    if (omitted(action.*field.member)) continue;
    const char *group = group_name(action, field);
    if (!groups.insert(group).second) {
      throw std::runtime_error("duplicate action payload group " + std::string(group));
    }
    present.push_back(&field);
  }

  msgpack::sbuffer buffer;
  buffer.write("\x01", 1);
  Packer packer(&buffer);
  packer.pack_map(static_cast<std::uint32_t>(present.size()));
  for (const auto *field : present) {
    pack_string(packer, group_name(action, *field));
    const Value &value = action.*field->member;
    if ((value.t == VType::Dict || value.t == VType::Obj) && value.fields) {
      pack_fields(packer, *value.fields, field->legacy);
    } else {
      pack_value(packer, value);
    }
  }
  return std::string(buffer.data(), buffer.size());
}

}  // namespace mch
