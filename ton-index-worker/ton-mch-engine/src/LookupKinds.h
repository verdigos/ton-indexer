#pragma once

#include <array>
#include <string_view>

namespace mch {

enum class LookupHandler {
  JettonWallet,
  NftItem,
  NftSale,
  NftAuction,
  DedustPool,
  NominatorPool,
  MultisigOrder,
  TelemintItem,
  JvaultAssets,
  CodeHash,
};

struct LookupKind {
  std::string_view name;
  LookupHandler handler;
  bool tier1;
};

inline constexpr std::array kLookupKindTable{
    LookupKind{"jetton_wallet", LookupHandler::JettonWallet, true},
    LookupKind{"nft_item", LookupHandler::NftItem, true},
    LookupKind{"nft_sale", LookupHandler::NftSale, true},
    LookupKind{"nft_auction", LookupHandler::NftAuction, true},
    LookupKind{"dedust_pool", LookupHandler::DedustPool, true},
    LookupKind{"nominator_pool", LookupHandler::NominatorPool, true},
    LookupKind{"multisig_order", LookupHandler::MultisigOrder, true},
    LookupKind{"telemint_item", LookupHandler::TelemintItem, true},
    LookupKind{"jvault_assets", LookupHandler::JvaultAssets, false},
    LookupKind{"code_hash", LookupHandler::CodeHash, false},
};

constexpr const LookupKind *lookup_kind(std::string_view name) {
  for (const auto &kind : kLookupKindTable) {
    if (kind.name == name) return &kind;
  }
  return nullptr;
}

constexpr bool lookup_kind_table_valid() {
  for (std::size_t i = 0; i < kLookupKindTable.size(); ++i) {
    const auto &kind = kLookupKindTable[i];
    if (kind.name.empty()) return false;
    for (std::size_t j = i + 1; j < kLookupKindTable.size(); ++j) {
      if (kind.name == kLookupKindTable[j].name) return false;
    }
  }
  return true;
}

static_assert(lookup_kind_table_valid());

}  // namespace mch
