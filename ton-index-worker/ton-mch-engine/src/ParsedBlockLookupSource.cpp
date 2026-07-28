#include "ParsedBlockLookupSource.h"

#include "Value.h"

#include "convert-utils.h"  // convert::to_raw_address(block::StdAddress)

#include <variant>

namespace mch {

namespace {

// StdAddress -> the canonical uppercase raw "wc:HEX" string the fixtures store
// and normalize_raw_address produces (SchemaTraceLoader normalises tx accounts
// the same way). Interface addresses render as Str (VType::Str), matching
// FixtureLookupSource's iface_obj which copies the fixture's raw string verbatim.
Value addr_str(const block::StdAddress &a) {
  std::string raw = convert::to_raw_address(a);
  if (auto norm = normalize_raw_address(raw)) {
    return Value::make_str(*norm);
  }
  return Value::make_str(raw);
}

Value addr_str_opt(const std::optional<block::StdAddress> &a) {
  return a ? addr_str(*a) : Value::null();
}

// A dedust pool asset element: {is_ton, address} — the two fields
// HostDedust.cpp reads. nullopt asset slot == TON.
Value dedust_asset(const std::optional<block::StdAddress> &a) {
  Value::Fields f;
  f.emplace_back("is_ton", Value::make_bool(!a.has_value()));
  f.emplace_back("address", addr_str_opt(a));
  return Value::make_obj(std::move(f));
}

}  // namespace

const std::set<std::string> &ParsedBlockLookupSource::kinds() {
  // Same set FixtureLookupSource serves — keeps prepare_classify's skip table
  // byte-identical on the production path.
  return FixtureLookupSource::kinds();
}

Value ParsedBlockLookupSource::tier1(const std::string &kind, const std::string &addr) const {
  if (account_interfaces_ == nullptr) {
    return Value::null();
  }
  auto r_addr = block::StdAddress::parse(td::Slice(addr));
  if (r_addr.is_error()) {
    return Value::null();  // malformed key: clean miss
  }
  auto it = account_interfaces_->find(r_addr.move_as_ok());
  if (it == account_interfaces_->end()) {
    return Value::null();
  }
  for (const schema::BlockchainInterfaceV2 &iface : it->second) {
    if (kind == "jetton_wallet") {
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
    } else if (kind == "nft_item") {
      if (const auto *nft = std::get_if<schema::NFTItemDataV2>(&iface)) {
        // Obj{address, init, index, collection_address, owner_address, content}.
        Value::Fields f;
        f.emplace_back("address", addr_str(nft->address));
        f.emplace_back("init", Value::make_bool(nft->init));
        f.emplace_back("index",
                       nft->index.is_null() ? Value::null() : Value::make_int(nft->index));
        f.emplace_back("collection_address", addr_str_opt(nft->collection_address));
        f.emplace_back("owner_address", addr_str_opt(nft->owner_address));
        // content (metadata map) is not modelled as a Value and is never read by
        // the nft_item consumers (dns reads collection_address only) — Null.
        f.emplace_back("content", Value::null());
        return Value::make_obj(std::move(f));
      }
    } else if (kind == "nft_sale") {
      // Two GetGems sale variants share the fields the nft host fn reads
      // (marketplace_address / nft_address / full_price / nft_owner_address).
      if (const auto *s = std::get_if<schema::GetGemsNftFixPriceSaleData>(&iface)) {
        Value::Fields f;
        f.emplace_back("marketplace_address", addr_str(s->marketplace_address));
        f.emplace_back("nft_address", addr_str(s->nft_address));
        f.emplace_back("full_price",
                       s->full_price.is_null() ? Value::null() : Value::make_int(s->full_price));
        f.emplace_back("nft_owner_address", addr_str_opt(s->nft_owner_address));
        return Value::make_obj(std::move(f));
      }
      if (const auto *s = std::get_if<schema::GetGemsNftFixPriceSaleV4Data>(&iface)) {
        Value::Fields f;
        f.emplace_back("marketplace_address", addr_str(s->marketplace_address));
        f.emplace_back("nft_address", addr_str(s->nft_address));
        f.emplace_back("full_price",
                       s->full_price.is_null() ? Value::null() : Value::make_int(s->full_price));
        f.emplace_back("nft_owner_address", addr_str_opt(s->nft_owner_address));
        return Value::make_obj(std::move(f));
      }
    } else if (kind == "nft_auction") {
      if (const auto *a = std::get_if<schema::GetGemsNftAuctionData>(&iface)) {
        // Obj{mp_addr, nft_addr, last_bid, nft_owner}.
        Value::Fields f;
        f.emplace_back("mp_addr", addr_str(a->mp_addr));
        f.emplace_back("nft_addr", addr_str(a->nft_addr));
        f.emplace_back("last_bid",
                       a->last_bid.is_null() ? Value::null() : Value::make_int(a->last_bid));
        f.emplace_back("nft_owner", addr_str_opt(a->nft_owner));
        return Value::make_obj(std::move(f));
      }
    } else if (kind == "dedust_pool") {
      if (const auto *p = std::get_if<schema::DedustPoolData>(&iface)) {
        // Obj{assets: [ {is_ton, address}, ... ]} — HostDedust reads is_ton +
        // address per element; a nullopt asset slot is the TON side.
        std::vector<Value> assets;
        assets.push_back(dedust_asset(p->asset_1));
        assets.push_back(dedust_asset(p->asset_2));
        Value::Fields f;
        f.emplace_back("assets", Value::make_list(std::move(assets)));
        return Value::make_obj(std::move(f));
      }
    }
  }
  return Value::null();  // account present, no matching variant: miss
}

Value ParsedBlockLookupSource::fetch(const std::string &kind, const std::vector<Value> &args) const {
  // FixtureLookupSource parity guard: every registered lookup passes exactly one
  // Str address; anything else is a null result on both sources.
  if (args.size() != 1 || args[0].t != VType::Str) {
    return Value::null();
  }
  // Tier 1 resolves from account_interfaces_; a miss falls through to tier 2.
  // No kind whitelist here: tier1() self-gates (every branch tests `kind ==`,
  // an unhandled kind falls through to null), so a second hand-kept kind list
  // could only drift out of sync with it.
  Value v = tier1(kind, args[0].str);
  if (!v.is_null()) {
    stats_.tier1_hits++;
    return v;
  }
  // Tier-2-only kinds (nominator_pool, jvault_assets) and every tier-1 miss.
  Value r = tier2_ ? tier2_(kind, args) : Value::null();
  if (!r.is_null()) {
    stats_.tier2_hits++;
  } else {
    stats_.misses++;
    stats_.misses_by_kind[kind]++;
  }
  return r;
}

}  // namespace mch
