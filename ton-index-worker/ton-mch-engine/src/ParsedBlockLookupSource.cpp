#include "ParsedBlockLookupSource.h"

#include "Value.h"

#include "convert-utils.h"  // convert::to_raw_address(block::StdAddress)
#include "td/utils/misc.h"

#include <map>
#include <variant>

namespace mch {

namespace {
enum class InterfaceHandler {
  JettonWallet,
  NftItem,
  NftSale,
  NftAuction,
  DedustPool,
  NominatorPool,
  Vesting,
  MultisigOrder,
  TelemintItem,
};

// These lookup kinds come from the variants handled by BlockInterfacesDetector.
const std::map<std::string, InterfaceHandler> kInterfaceHandlers = {
    {"jetton_wallet", InterfaceHandler::JettonWallet},
    {"nft_item", InterfaceHandler::NftItem},
    {"nft_sale", InterfaceHandler::NftSale},
    {"nft_auction", InterfaceHandler::NftAuction},
    {"dedust_pool", InterfaceHandler::DedustPool},
    {"nominator_pool", InterfaceHandler::NominatorPool},
    {"vesting", InterfaceHandler::Vesting},
    {"multisig_order", InterfaceHandler::MultisigOrder},
    {"telemint_item", InterfaceHandler::TelemintItem},
};

// StdAddress -> the canonical uppercase raw "wc:HEX" string the fixtures store
// and normalize_raw_address produces (SchemaTraceLoader normalises tx accounts
// the same way). Interface addresses render as Str (VType::Str), matching
// FixtureLookupSource's iface_obj which copies the fixture's raw string verbatim.
Value addr_str(const block::StdAddress &a) {
  std::string raw = convert::to_raw_address(a);
  return Value::make_str(canonicalize_or_passthrough(raw));
}

Value addr_str_opt(const std::optional<block::StdAddress> &a) {
  return a ? addr_str(*a) : Value::null();
}

// `code_hash` is base64 of the account's code-cell hash, compared against
// literal b64 constants. An all-zero Bits256 is the emulator adapter's
// value-initialized tail, never a real code hash, so it renders as null.
// This is a miss the reader can reject on, not a b64 string that would
// silently pick the "latest" parser branch.
Value code_hash_str(const td::Bits256 &h) {
  if (h.is_zero()) {
    return Value::null();
  }
  return Value::make_str(td::base64_encode(h.as_slice()));
}

// NftItem.content must be a Dict because get_nft_data tests the top-level `uri`
// key and faults on another type. A flat string map preserves the observable
// fields; nested metadata is not read by matchers or action fills. Missing content
// remains null so it rejects instead of fabricating an empty record.
Value nft_content(const std::optional<std::map<std::string, std::string>> &content) {
  if (!content) {
    return Value::null();
  }
  Value::Fields f;
  for (const auto &[key, val] : *content) {
    f.emplace_back(key, Value::make_str(val));
  }
  return Value::make_dict(std::move(f));
}

// A DeDust pool asset element: {is_ton, address}, the two fields
// HostDedust.cpp reads. nullopt asset slot == TON.
Value dedust_asset(const std::optional<block::StdAddress> &a) {
  Value::Fields f;
  f.emplace_back("is_ton", Value::make_bool(!a.has_value()));
  f.emplace_back("address", addr_str_opt(a));
  return Value::make_obj(std::move(f));
}

}  // namespace

Value ParsedBlockLookupSource::iface_value(const std::string &kind,
                                           const schema::BlockchainInterfaceV2 &iface) {
  auto handler = kInterfaceHandlers.find(kind);
  if (handler == kInterfaceHandlers.end()) {
    return Value::null();
  }
  switch (handler->second) {
    case InterfaceHandler::JettonWallet:
      if (const auto *jw = std::get_if<schema::JettonWalletDataV2>(&iface)) {
        // FixtureLookupSource shape: Obj{balance, address, owner, jetton}.
        Value::Fields f;
        f.emplace_back("balance", jw->balance.is_null() ? Value::null()
                                                        : Value::make_int(jw->balance));
        f.emplace_back("address", addr_str(jw->address));
        f.emplace_back("owner", addr_str(jw->owner));
        f.emplace_back("jetton", addr_str(jw->jetton));
        return Value::make_obj(std::move(f));
      }
      break;
    case InterfaceHandler::NftItem:
      if (const auto *nft = std::get_if<schema::NFTItemDataV2>(&iface)) {
        // Obj{address, init, index, collection_address, owner_address, content,
        // code_hash}.
        Value::Fields f;
        f.emplace_back("address", addr_str(nft->address));
        f.emplace_back("init", Value::make_bool(nft->init));
        f.emplace_back("index",
                       nft->index.is_null() ? Value::null() : Value::make_int(nft->index));
        f.emplace_back("collection_address", addr_str_opt(nft->collection_address));
        f.emplace_back("owner_address", addr_str_opt(nft->owner_address));
        f.emplace_back("content", nft_content(nft->content));
        f.emplace_back("code_hash", code_hash_str(nft->code_hash));
        return Value::make_obj(std::move(f));
      }
      break;
    case InterfaceHandler::NftSale:
      // Two GetGems sale variants share the fields the nft host fns read
      // (marketplace_address / nft_address / full_price / nft_owner_address /
      // code_hash).
      if (const auto *s = std::get_if<schema::GetGemsNftFixPriceSaleData>(&iface)) {
        Value::Fields f;
        f.emplace_back("marketplace_address", addr_str(s->marketplace_address));
        f.emplace_back("nft_address", addr_str(s->nft_address));
        f.emplace_back("full_price",
                       s->full_price.is_null() ? Value::null() : Value::make_int(s->full_price));
        f.emplace_back("nft_owner_address", addr_str_opt(s->nft_owner_address));
        f.emplace_back("code_hash", code_hash_str(s->code_hash));
        return Value::make_obj(std::move(f));
      }
      if (const auto *s = std::get_if<schema::GetGemsNftFixPriceSaleV4Data>(&iface)) {
        Value::Fields f;
        f.emplace_back("marketplace_address", addr_str(s->marketplace_address));
        f.emplace_back("nft_address", addr_str(s->nft_address));
        f.emplace_back("full_price",
                       s->full_price.is_null() ? Value::null() : Value::make_int(s->full_price));
        f.emplace_back("nft_owner_address", addr_str_opt(s->nft_owner_address));
        f.emplace_back("code_hash", code_hash_str(s->code_hash));
        return Value::make_obj(std::move(f));
      }
      break;
    case InterfaceHandler::NftAuction:
      if (const auto *a = std::get_if<schema::GetGemsNftAuctionData>(&iface)) {
        // Obj{mp_addr, nft_addr, last_bid, nft_owner, code_hash}.
        Value::Fields f;
        f.emplace_back("mp_addr", addr_str(a->mp_addr));
        f.emplace_back("nft_addr", addr_str(a->nft_addr));
        f.emplace_back("last_bid",
                       a->last_bid.is_null() ? Value::null() : Value::make_int(a->last_bid));
        f.emplace_back("nft_owner", addr_str_opt(a->nft_owner));
        f.emplace_back("code_hash", code_hash_str(a->code_hash));
        return Value::make_obj(std::move(f));
      }
      break;
    case InterfaceHandler::DedustPool:
      if (const auto *p = std::get_if<schema::DedustPoolData>(&iface)) {
        // Obj{assets: [ {is_ton, address}, ... ]}, HostDedust reads is_ton +
        // address per element; a nullopt asset slot is the TON side.
        std::vector<Value> assets;
        assets.push_back(dedust_asset(p->asset_1));
        assets.push_back(dedust_asset(p->asset_2));
        Value::Fields f;
        f.emplace_back("assets", Value::make_list(std::move(assets)));
        return Value::make_obj(std::move(f));
      }
      break;
    case InterfaceHandler::NominatorPool:
      if (const auto *p = std::get_if<schema::NominatorPoolData>(&iface)) {
        // Obj{address}. The whole semantics is set membership: the one spec
        // that reads this lookup rejects on null and never touches a field
        // (ir/mch_ir.json nominator_pool_withdraw), which is also why Python's
        // registered fn just hands back the account's interfaces dict.
        Value::Fields f;
        f.emplace_back("address", addr_str(p->address));
        return Value::make_obj(std::move(f));
      }
      break;
    case InterfaceHandler::Vesting:
      if (const auto *p = std::get_if<schema::VestingData>(&iface)) {
        Value::Fields f;
        f.emplace_back("address", addr_str(p->address));
        return Value::make_obj(std::move(f));
      }
      break;
    case InterfaceHandler::MultisigOrder:
      if (const auto *p = std::get_if<schema::MultisigOrderData>(&iface)) {
        std::vector<Value> signers;
        for (const auto &s : p->signers) {
          signers.push_back(addr_str(s));
        }
        Value::Fields f;
        f.emplace_back("signers", Value::make_list(std::move(signers)));
        return Value::make_obj(std::move(f));
      }
      break;
    case InterfaceHandler::TelemintItem:
      if (const auto *p = std::get_if<schema::TelemintData>(&iface)) {
        Value::Fields f;
        f.emplace_back("address", addr_str(p->address));
        return Value::make_obj(std::move(f));
      }
      break;
  }
  return Value::null();  // no matching variant for this kind
}

std::optional<Value> ParsedBlockLookupSource::tier1(const std::string &kind,
                                                    const std::string &addr) const {
  if (account_interfaces_ == nullptr) {
    return std::nullopt;
  }
  auto r_addr = block::StdAddress::parse(td::Slice(addr));
  if (r_addr.is_error()) {
    return std::nullopt;  // malformed key: clean miss
  }
  auto it = account_interfaces_->find(r_addr.move_as_ok());
  if (it == account_interfaces_->end()) {
    return std::nullopt;
  }
  for (const schema::BlockchainInterfaceV2 &iface : it->second) {
    Value v = iface_value(kind, iface);
    if (!v.is_null()) {
      return v;
    }
  }
  return Value::null();  // account present, no matching variant
}

Value ParsedBlockLookupSource::fetch(const std::string &kind, const std::vector<Value> &args) const {
  // FixtureLookupSource agreement guard: every registered lookup passes exactly one
  // address, Str, or the Account a `.data` address field yields (same raw
  // uppercase text); anything else is a null result on both sources.
  if (args.size() != 1 ||
      !(args[0].t == VType::Str ||
        (args[0].t == VType::Account && !args[0].addr_none))) {
    return Value::null();
  }
  std::optional<Value> v = tier1(kind, args[0].str);
  if (v && !v->is_null()) {
    stats_.tier1_hits++;
    return std::move(*v);
  }
  if (v && status_stable_accounts_ != nullptr &&
      kInterfaceHandlers.find(kind) != kInterfaceHandlers.end()) {
    auto r_addr = block::StdAddress::parse(td::Slice(args[0].str));
    if (r_addr.is_ok() && status_stable_accounts_->count(r_addr.ok()) != 0) {
      stats_.misses++;
      stats_.misses_by_kind[kind]++;
      return Value::null();
    }
  }
  // Tier-2-only kinds and every non-authoritative tier-1 miss fall through.
  auto result = tier2_ ? tier2_(kind, args) : Tier2Result{Value::null(), false};
  history_unavailable_ |= result.unavailable;
  Value r = std::move(result.value);
  if (!r.is_null()) {
    stats_.tier2_hits++;
    stats_.tier2_hits_by_kind[kind]++;
  } else {
    stats_.misses++;
    stats_.misses_by_kind[kind]++;
  }
  return r;
}

}  // namespace mch
