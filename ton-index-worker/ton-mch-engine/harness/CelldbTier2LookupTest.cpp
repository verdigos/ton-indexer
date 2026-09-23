// Cell-db tier-2 tests pin value-shape equality with tier 1, JVault cell parsing,
// memoization, and argument discipline. Injected account reads cannot exercise
// successful get-method detection or the final JVault jetton-wallet leg.
#include "CelldbTier2LookupTest.h"

#include "TieredLookupSource.h"
#include "Value.h"
#include "CelldbTier2Lookup.h"
#include "DetectorInterfaceAdapter.h"
#include "JvaultLookup.h"
#include "fixtures/Render.h"

#include "common/refint.h"
#include "vm/cells/CellBuilder.h"

#include <cstdio>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace mch {

namespace {

int g_fail = 0;

void check(const std::string &name, bool ok) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
  if (!ok) {
    g_fail++;
  }
}

block::StdAddress addr_of(char fill) {
  return block::StdAddress::parse(td::Slice("0:" + std::string(64, fill))).move_as_ok();
}

std::string raw_of(char fill) {
  return "0:" + std::string(64, fill);
}

// A shard-state stand-in: never dereferenced (the injected account source is what
// answers), but non-empty, because an empty state vector is the product's
// "listener trace, no tier 2" short-circuit.
AllShardStates dummy_states() {
  vm::CellBuilder cb;
  return AllShardStates{cb.finalize()};
}

// Synthetic account-data cells
// addr_std$10 anycast:(##0) workchain:int8 address:bits256, i.e. what
// load_address_value reads back.
void store_addr(vm::CellBuilder &cb, const block::StdAddress &a) {
  cb.store_long(2, 2);   // addr_std$10
  cb.store_long(0, 1);   // anycast absent
  cb.store_long(a.workchain, 8);
  cb.store_bits(a.addr.cbits(), 256);
}

void store_addr_none(vm::CellBuilder &cb) {
  cb.store_long(0, 2);  // addr_none$00
}

// jvault stake wallet data: staking_pool then minter, no header.
td::Ref<vm::Cell> stake_wallet_data(const std::optional<block::StdAddress> &pool,
                                    const std::optional<block::StdAddress> &minter) {
  vm::CellBuilder cb;
  pool ? store_addr(cb, *pool) : store_addr_none(cb);
  minter ? store_addr(cb, *minter) : store_addr_none(cb);
  return cb.finalize();
}

// jvault staking pool data: 1+32 header bits, then admin, creator, lock wallet.
td::Ref<vm::Cell> stake_pool_data(const block::StdAddress &admin, const block::StdAddress &creator,
                                  const block::StdAddress &lock) {
  vm::CellBuilder cb;
  cb.store_long(0, 1);
  cb.store_long(0, 32);
  store_addr(cb, admin);
  store_addr(cb, creator);
  store_addr(cb, lock);
  return cb.finalize();
}

schema::AccountState account_with_data(const block::StdAddress &a, td::Ref<vm::Cell> data) {
  schema::AccountState st{};
  st.account = a;
  st.data = std::move(data);
  vm::CellBuilder code;
  code.store_long(0, 8);
  st.code = code.finalize();
  return st;
}

// Tier shape equality

// Render `iface` the way TIER 1 does: put it in an account_interfaces_ map and
// go through a real TieredLookupSource::fetch.
Value via_tier1(const std::string &kind, const block::StdAddress &addr,
                const schema::BlockchainInterfaceV2 &iface) {
  TieredLookupSource::InterfaceMap map;
  map.emplace(addr, std::vector<schema::BlockchainInterfaceV2>{iface});
  TieredLookupSource src(&map);
  return src.fetch(kind, {Value::make_str(std::to_string(addr.workchain) + ":" + addr.addr.to_hex())});
}

void test_tier_shapes() {
  const block::StdAddress a = addr_of('A');
  const block::StdAddress b = addr_of('B');
  const block::StdAddress c = addr_of('C');

  auto same = [&](const std::string &kind, const schema::BlockchainInterfaceV2 &iface) {
    Value t1 = via_tier1(kind, a, iface);
    Value t2 = TieredLookupSource::iface_value(kind, iface);
    check("tier_shape/" + kind + "/nonnull", !t2.is_null());
    check("tier_shape/" + kind + "/equal", structural_equal(t1, t2));
  };

  {
    schema::JettonWalletDataV2 jw{};
    jw.balance = td::make_refint(12345);
    jw.address = a;
    jw.owner = b;
    jw.jetton = c;
    same("jetton_wallet", jw);
  }
  {
    schema::NFTItemDataV2 nft{};
    nft.address = a;
    nft.init = true;
    nft.index = td::make_refint(7);
    nft.collection_address = b;
    nft.owner_address = c;
    // Content must render as a Dict on both tiers. get_nft_data faults
    // on a non-Dict and a fault drops the whole nft_transfer row.
    nft.content = std::map<std::string, std::string>{{"uri", "https://x/y.json"}};
    same("nft_item", nft);
    Value v = TieredLookupSource::iface_value("nft_item", nft);
    const Value *content = v.field("content");
    check("tier_shape/nft_item/content_is_dict",
          content != nullptr && content->t == VType::Dict);
  }
  {
    schema::GetGemsNftFixPriceSaleData s{};
    s.address = a;
    s.marketplace_address = b;
    s.nft_address = c;
    s.full_price = td::make_refint(999);
    s.nft_owner_address = b;
    same("nft_sale", s);
  }
  {
    // The V4 variant answers the SAME kind, that is why widening the emulator's
    // detector set needs no tier-1 change.
    schema::GetGemsNftFixPriceSaleV4Data s{};
    s.address = a;
    s.marketplace_address = b;
    s.nft_address = c;
    s.full_price = td::make_refint(1000);
    s.nft_owner_address = c;
    same("nft_sale", s);
  }
  {
    schema::GetGemsNftAuctionData s{};
    s.address = a;
    s.mp_addr = b;
    s.nft_addr = c;
    s.last_bid = td::make_refint(42);
    s.nft_owner = b;
    same("nft_auction", s);
  }
  {
    schema::DedustPoolData p{};
    p.address = a;
    p.asset_1 = std::nullopt;  // the TON side
    p.asset_2 = b;
    same("dedust_pool", p);
    Value v = TieredLookupSource::iface_value("dedust_pool", p);
    const Value *assets = v.field("assets");
    check("tier_shape/dedust_pool/two_assets",
          assets != nullptr && assets->t == VType::List && assets->items != nullptr &&
              assets->items->size() == 2);
    if (assets != nullptr && assets->items != nullptr && assets->items->size() == 2) {
      const Value *is_ton = (*assets->items)[0].field("is_ton");
      check("tier_shape/dedust_pool/ton_slot_is_ton",
            is_ton != nullptr && is_ton->t == VType::Bool && is_ton->boolean);
    }
  }
  {
    schema::NominatorPoolData p{};
    p.address = a;
    same("nominator_pool", p);
    Value v = TieredLookupSource::iface_value("nominator_pool", p);
    const Value *ad = v.field("address");
    check("tier_shape/nominator_pool/address_only",
          ad != nullptr && ad->t == VType::Str && v.fields != nullptr && v.fields->size() == 1);
  }
  {
    schema::TelemintData p{};
    p.address = a;
    same("telemint_item", p);
    Value v = TieredLookupSource::iface_value("telemint_item", p);
    const Value *ad = v.field("address");
    check("tier_shape/telemint_item/address_only",
          ad != nullptr && ad->t == VType::Str && v.fields != nullptr && v.fields->size() == 1);
  }
  {
    // A variant that answers no kind must render null, not an empty Obj, so
    // `reject when lookup == null` continues to fire.
    schema::JettonWalletDataV2 jw{};
    jw.address = a;
    check("tier_shape/wrong_kind_is_null",
          TieredLookupSource::iface_value("dedust_pool", jw).is_null());
  }
}

// NFT code hash

// Detector Results carry no code hash, so tier 2 fills it from the account state
// it ran the detector on. Detection itself needs a real config, out of reach
// here, so this pins the adapter the tier-2 path calls on its way to rendering.
void test_nft_code_hash() {
  const block::StdAddress a = addr_of('A');
  const schema::AccountState st = account_with_data(a, stake_wallet_data(a, addr_of('B')));
  const td::Bits256 from_cell{st.code->get_hash().bits()};

  auto rendered_hash = [](const std::string &kind, schema::BlockchainInterfaceV2 iface,
                          const schema::AccountState &state) {
    set_nft_code_hash(iface, state);
    Value v = TieredLookupSource::iface_value(kind, iface);
    const Value *h = v.field("code_hash");
    return h == nullptr ? Value::null() : *h;
  };

  schema::NFTItemDataV2 nft{};
  nft.address = a;
  schema::GetGemsNftFixPriceSaleData sale{};
  sale.address = a;
  schema::GetGemsNftFixPriceSaleV4Data sale_v4{};
  sale_v4.address = a;
  schema::GetGemsNftAuctionData auction{};
  auction.address = a;

  const std::vector<std::tuple<std::string, std::string, schema::BlockchainInterfaceV2>> shapes{
      {"nft_item", "nft_item", nft},
      {"nft_sale", "nft_sale", sale},
      {"nft_sale_v4", "nft_sale", sale_v4},
      {"nft_auction", "nft_auction", auction},
  };
  const std::string expected = td::base64_encode(from_cell.as_slice());
  for (const auto &[name, kind, iface] : shapes) {
    Value h = rendered_hash(kind, iface, st);
    check("code_hash/" + name + "/from_code_cell", h.t == VType::Str && h.str == expected);
  }

  {
    // A state that already carries the parsed hash is taken verbatim.
    schema::AccountState parsed = st;
    parsed.code_hash = addr_of('7').addr;
    Value h = rendered_hash("nft_item", nft, parsed);
    check("code_hash/parsed_hash_wins",
          h.t == VType::Str && h.str == td::base64_encode(parsed.code_hash.value().as_slice()));
  }
  {
    // No code cell leaves the hash zero, which renders as a miss.
    schema::AccountState codeless{};
    codeless.account = a;
    check("code_hash/no_code_is_null", rendered_hash("nft_item", nft, codeless).is_null());
  }
  {
    // Variants with no code-hash consumer stay untouched.
    schema::BlockchainInterfaceV2 pool = schema::DedustPoolData{};
    set_nft_code_hash(pool, st);
    check("code_hash/other_variant_untouched",
          std::get<schema::DedustPoolData>(pool).code_hash.is_zero());
  }
}

// nominator_pool code-hash predicate

void test_nominator_predicate() {
  const block::StdAddress a = addr_of('A');
  vm::CellBuilder cb;
  cb.store_long(0, 8);
  auto not_the_pool = cb.finalize();
  check("nominator/wrong_code_hash_rejects",
        NominatorPoolContract::detect(a, not_the_pool, not_the_pool).is_error());
  check("nominator/null_code_rejects",
        NominatorPoolContract::detect(a, td::Ref<vm::Cell>{}, not_the_pool).is_error());
  check("nominator/null_data_rejects",
        NominatorPoolContract::detect(a, not_the_pool, td::Ref<vm::Cell>{}).is_error());
}

// JVault data-cell chain

void test_jvault_parsers() {
  const block::StdAddress pool = addr_of('1');
  const block::StdAddress minter = addr_of('2');
  const block::StdAddress lock = addr_of('3');

  {
    auto r = parse_jvault_stake_wallet(stake_wallet_data(pool, minter));
    check("jvault/wallet_ok", r.is_ok());
    if (r.is_ok()) {
      auto p = r.move_as_ok();
      check("jvault/wallet_pool", p.staking_pool_raw == raw_of('1'));
      check("jvault/wallet_minter", p.minter.has_value() && *p.minter == raw_of('2'));
      check("jvault/wallet_pool_is_account", p.staking_pool.t == VType::Account);
    }
  }
  {
    // addr_none pool: the whole record is rejected, not a partial one.
    check("jvault/wallet_addr_none_pool_rejects",
          parse_jvault_stake_wallet(stake_wallet_data(std::nullopt, minter)).is_error());
  }
  {
    // addr_none minter is NOT fatal: it only leaves jvault_asset null.
    auto r = parse_jvault_stake_wallet(stake_wallet_data(pool, std::nullopt));
    check("jvault/wallet_addr_none_minter_ok", r.is_ok());
    if (r.is_ok()) {
      check("jvault/wallet_addr_none_minter_null", !r.move_as_ok().minter.has_value());
    }
  }
  {
    vm::CellBuilder cb;
    cb.store_long(2, 2);  // a truncated addr_std
    check("jvault/wallet_truncated_rejects", parse_jvault_stake_wallet(cb.finalize()).is_error());
    check("jvault/wallet_null_cell_rejects",
          parse_jvault_stake_wallet(td::Ref<vm::Cell>{}).is_error());
  }
  {
    auto r = parse_jvault_pool_lock_wallet(stake_pool_data(minter, pool, lock));
    check("jvault/pool_lock_wallet_is_third", r.is_ok() && r.move_as_ok() == raw_of('3'));
  }
  {
    // Same three addresses without the 1+32 header: the parse must not silently
    // succeed on a shifted cell.
    vm::CellBuilder cb;
    store_addr(cb, minter);
    store_addr(cb, pool);
    store_addr(cb, lock);
    auto r = parse_jvault_pool_lock_wallet(cb.finalize());
    check("jvault/pool_without_header_not_third", !r.is_ok() || r.move_as_ok() != raw_of('3'));
  }
  {
    vm::CellBuilder cb;
    cb.store_long(0, 1);
    cb.store_long(0, 32);
    check("jvault/pool_short_rejects", parse_jvault_pool_lock_wallet(cb.finalize()).is_error());
  }
}

// A tier-2 instance over a fixed account map. config is null on purpose: the
// jvault chain needs none, and the jetton_wallet leg then degrades exactly as it
// would when the detector fails.
struct Fixture {
  AllShardStates states = dummy_states();
  std::map<std::string, schema::AccountState> accounts;
  int reads = 0;

  CelldbTier2Lookup make() {
    CelldbTier2Lookup t(&states, nullptr);
    t.set_account_source([this](const block::StdAddress &a) -> td::Result<std::optional<schema::AccountState>> {
      reads++;
      auto it = accounts.find(std::to_string(a.workchain) + ":" + a.addr.to_hex());
      if (it == accounts.end()) {
        return std::optional<schema::AccountState>{};
      }
      return std::optional<schema::AccountState>{it->second};
    });
    return t;
  }
  void put(char fill, td::Ref<vm::Cell> data) {
    accounts.emplace(raw_of(fill), account_with_data(addr_of(fill), std::move(data)));
  }
};

void test_jvault_hook() {
  const block::StdAddress pool = addr_of('1');
  const block::StdAddress minter = addr_of('2');
  const block::StdAddress lock = addr_of('3');
  const std::vector<Value> wallet_arg{Value::make_str(raw_of('9'))};

  {
    Fixture f;  // stake wallet absent
    auto t = f.make();
    check("jvault_hook/wallet_absent_null", t.fetch("jvault_assets", wallet_arg).is_null());
  }
  {
    Fixture f;  // stake wallet present, pool state absent -> PARTIAL record
    f.put('9', stake_wallet_data(pool, minter));
    auto t = f.make();
    Value v = t.fetch("jvault_assets", wallet_arg);
    const Value *sp = v.field("staking_pool");
    const Value *asset = v.field("asset");
    const Value *jv = v.field("jvault_asset");
    check("jvault_hook/partial_is_obj", v.t == VType::Obj);
    check("jvault_hook/partial_pool", sp != nullptr && sp->t == VType::Account);
    check("jvault_hook/partial_asset_null", asset != nullptr && asset->is_null());
    check("jvault_hook/partial_jvault_asset", jv != nullptr && !jv->is_null());
  }
  {
    Fixture f;  // pool present but its data is not the pool layout -> whole null
    f.put('9', stake_wallet_data(pool, minter));
    vm::CellBuilder junk;
    junk.store_long(0, 8);
    f.put('1', junk.finalize());
    auto t = f.make();
    check("jvault_hook/pool_data_malformed_null",
          t.fetch("jvault_assets", wallet_arg).is_null());
  }
  {
    Fixture f;  // full chain, but the lock wallet is not a jetton wallet
    f.put('9', stake_wallet_data(pool, minter));
    f.put('1', stake_pool_data(minter, pool, lock));
    vm::CellBuilder junk;
    junk.store_long(0, 8);
    f.put('3', junk.finalize());
    auto t = f.make();
    check("jvault_hook/lock_not_jetton_wallet_null",
          t.fetch("jvault_assets", wallet_arg).is_null());
  }
}

// Memo and argument discipline

void test_memo_and_guards() {
  {
    Fixture f;
    f.put('A', stake_wallet_data(addr_of('1'), addr_of('2')));
    auto t = f.make();
    const std::vector<Value> arg{Value::make_str(raw_of('A'))};
    t.fetch("nominator_pool", arg);
    t.fetch("nominator_pool", arg);
    check("memo/one_account_read", f.reads == 1);
    check("memo/hit_counted", t.stats().memo_hits == 1);
    check("memo/fetched_counted", t.stats().fetched == 1);
  }
  {
    // Case folding: block::StdAddress::parse normalises, so an upper- and a
    // lower-case spelling of one address must share the memo entry.
    Fixture f;
    f.put('A', stake_wallet_data(addr_of('1'), addr_of('2')));
    auto t = f.make();
    t.fetch("nominator_pool", {Value::make_str("0:" + std::string(64, 'A'))});
    t.fetch("nominator_pool", {Value::make_str("0:" + std::string(64, 'a'))});
    check("memo/case_insensitive", f.reads == 1 && t.stats().memo_hits == 1);
  }
  {
    // The kind is part of the key: two kinds over one address are two value-memo
    // entries (no memo hit), but share the ONE account read.
    Fixture f;
    f.put('A', stake_wallet_data(addr_of('1'), addr_of('2')));
    auto t = f.make();
    const std::vector<Value> arg{Value::make_str(raw_of('A'))};
    t.fetch("nominator_pool", arg);
    t.fetch("dedust_pool", arg);
    check("memo/kind_in_key", t.stats().memo_hits == 0 && f.reads == 1);
  }
  {
    // Mirrors the tier-1 dispatch contract
    Fixture f;
    auto t = f.make();
    check("guard/non_str_arg_null",
          t.fetch("nominator_pool", {Value::make_int(td::make_refint(1))}).is_null());
    check("guard/wrong_arity_null", t.fetch("nominator_pool", {}).is_null());
    check("guard/account_none_null",
          t.fetch("nominator_pool", {Value::make_account_none()}).is_null());
    check("guard/malformed_addr_null",
          t.fetch("nominator_pool", {Value::make_str("not-an-address")}).is_null());
    check("guard/unknown_kind_null",
          t.fetch("no_such_kind", {Value::make_str(raw_of('A'))}).is_null());
    check("guard/no_reads_on_guarded", f.reads == 0 && t.stats().fetched == 0);
    // An Account-typed argument must pass the guard and reach the account
    // read.
    t.fetch("nominator_pool", {Value::make_account_raw(raw_of('A'))});
    check("guard/account_arg_reaches_read", f.reads == 1);
  }
  for (const std::string kind : {"nft_item", "nft_auction"}) {
    Fixture f;
    auto t = f.make();
    check("dispatch/" + kind + "/absent_is_null",
          t.fetch(kind, {Value::make_str(raw_of('A'))}).is_null());
    check("dispatch/" + kind + "/reaches_account_read",
          f.reads == 1 && t.stats().fetched == 1);
  }
  {
    // A listener-emulated trace has no shard states: a plain miss, no read.
    AllShardStates none;
    CelldbTier2Lookup t(&none, nullptr);
    check("guard/no_shard_states_null",
          t.fetch("nominator_pool", {Value::make_str(raw_of('A'))}).is_null());
    check("guard/no_shard_states_no_read", t.stats().fetched == 0);
  }
}

void test_account_lookup_states() {
  const std::vector<Value> arg{Value::make_str(raw_of('A'))};
  for (int behavior = 0; behavior < 3; behavior++) {
    Fixture f;
    auto t = f.make();
    t.set_account_source([&](const block::StdAddress &)
                             -> td::Result<std::optional<schema::AccountState>> {
      f.reads++;
      if (behavior == 1) return td::Status::Error("read failed");
      if (behavior == 2) throw std::runtime_error("read threw");
      return std::optional<schema::AccountState>{};
    });
    TieredLookupSource src(nullptr, [&](const std::string &kind, const std::vector<Value> &args) {
      return t.fetch(kind, args);
    });
    const std::string name = behavior == 0 ? "account/absent/"
                                          : behavior == 1 ? "account/error/"
                                                          : "account/exception/";
    check(name + "null", src.fetch("nominator_pool", arg).is_null());
    check(name + "memoized_null", src.fetch("nominator_pool", arg).is_null());
    check(name + "one_read", f.reads == 1 && t.stats().memo_hits == 1);
    check(name + "account_memo_null", src.fetch("nft_item", arg).is_null() && f.reads == 1);
  }
}

// Two tiers behind one source

void test_two_tier_routing() {
  const block::StdAddress a = addr_of('A');
  schema::JettonWalletDataV2 jw{};
  jw.balance = td::make_refint(1);
  jw.address = a;
  jw.owner = addr_of('B');
  jw.jetton = addr_of('C');

  TieredLookupSource::InterfaceMap map;
  map.emplace(a, std::vector<schema::BlockchainInterfaceV2>{jw});

  int hook_calls = 0;
  TieredLookupSource::Tier2Hook hook = [&](const std::string &kind,
                                           const std::vector<Value> &) -> Value {
    hook_calls++;
    if (kind != "nominator_pool") {
      return Value::null();
    }
    schema::NominatorPoolData p{};
    p.address = addr_of('D');
    return TieredLookupSource::iface_value(kind, p);
  };
  TieredLookupSource src(&map, std::move(hook));

  Value t1 = src.fetch("jetton_wallet", {Value::make_str(raw_of('A'))});
  check("routing/tier1_wins", !t1.is_null() && hook_calls == 0);
  check("routing/tier1_counted", src.stats().tier1_hits == 1);

  Value t2 = src.fetch("nominator_pool", {Value::make_str(raw_of('D'))});
  check("routing/tier2_serves", !t2.is_null() && hook_calls == 1);
  check("routing/tier2_counted", src.stats().tier2_hits == 1);
  check("routing/tier2_kind_counted",
        src.stats().tier2_hits_by_kind.at("nominator_pool") == 1);
  // The tier-2 answer is the SAME shape tier 1 would have produced.
  schema::NominatorPoolData p{};
  p.address = addr_of('D');
  check("routing/tier2_shape_matches_tier1",
        structural_equal(t2, via_tier1("nominator_pool", addr_of('D'), p)));

  Value miss = src.fetch("dedust_pool", {Value::make_str(raw_of('E'))});
  check("routing/miss_attributed",
        miss.is_null() && src.stats().misses == 1 &&
            src.stats().misses_by_kind.at("dedust_pool") == 1);
}

void test_tier1_authoritative_miss() {
  const block::StdAddress a = addr_of('A');
  schema::JettonWalletDataV2 jw{};
  jw.balance = td::make_refint(1);
  jw.address = a;
  jw.owner = addr_of('B');
  jw.jetton = addr_of('C');

  TieredLookupSource::InterfaceMap map;
  map.emplace(a, std::vector<schema::BlockchainInterfaceV2>{jw});
  TieredLookupSource::StableAddressSet stable{a, addr_of('F')};
  TieredLookupSource::StableAddressSet unstable{addr_of('F')};

  int hook_calls = 0;
  TieredLookupSource::Tier2Hook hook =
      [&](const std::string &, const std::vector<Value> &)
      -> Value {
    hook_calls++;
    return Value::null();
  };
  TieredLookupSource src(&map, hook, &stable);
  TieredLookupSource src_unstable(&map, hook, &unstable);
  auto misses_for = [](const TieredLookupSource::LookupStats &stats,
                       const std::string &kind) {
    auto it = stats.misses_by_kind.find(kind);
    return it == stats.misses_by_kind.end() ? 0 : it->second;
  };
  auto is_miss_delta = [&](const TieredLookupSource &source,
                           const TieredLookupSource::LookupStats &before,
                           const std::string &kind) {
    const auto &after = source.stats();
    return after.misses == before.misses + 1 &&
           misses_for(after, kind) == misses_for(before, kind) + 1 &&
           after.tier1_hits == before.tier1_hits && after.tier2_hits == before.tier2_hits;
  };

  const auto before_authoritative = src.stats();
  const int calls_before_authoritative = hook_calls;
  Value authoritative = src.fetch("nft_item", {Value::make_str(raw_of('A'))});
  check("tier1_auth/authoritative_null",
        authoritative.is_null() && hook_calls == calls_before_authoritative &&
            is_miss_delta(src, before_authoritative, "nft_item"));

  const auto before_unstable = src_unstable.stats();
  const int calls_before_unstable = hook_calls;
  Value unstable_miss = src_unstable.fetch("nft_item", {Value::make_str(raw_of('A'))});
  check("tier1_auth/unstable_falls_through",
        unstable_miss.is_null() && hook_calls == calls_before_unstable + 1 &&
            is_miss_delta(src_unstable, before_unstable, "nft_item"));

  const auto before_non_interface = src.stats();
  const int calls_before_non_interface = hook_calls;
  Value non_interface = src.fetch("jetton_master", {Value::make_str(raw_of('A'))});
  check("tier1_auth/non_interface_kind_falls_through",
        non_interface.is_null() && hook_calls == calls_before_non_interface + 1 &&
            is_miss_delta(src, before_non_interface, "jetton_master"));

  const auto before_f = src.stats();
  const int calls_before_f = hook_calls;
  Value absent_stable = src.fetch("nft_item", {Value::make_str(raw_of('F'))});
  const auto after_f = src.stats();
  const bool stable_absent_falls_through =
      absent_stable.is_null() && hook_calls == calls_before_f + 1 &&
      is_miss_delta(src, before_f, "nft_item");
  const auto before_e = after_f;
  const int calls_before_e = hook_calls;
  Value absent_unstable = src.fetch("nft_item", {Value::make_str(raw_of('E'))});
  check("tier1_auth/absent_account_falls_through",
        stable_absent_falls_through && absent_unstable.is_null() &&
            hook_calls == calls_before_e + 1 && is_miss_delta(src, before_e, "nft_item"));

  const auto before_hit = src.stats();
  const int calls_before_hit = hook_calls;
  Value hit = src.fetch("jetton_wallet", {Value::make_str(raw_of('A'))});
  check("tier1_auth/tier1_hit_unaffected",
        !hit.is_null() && hook_calls == calls_before_hit &&
            src.stats().misses == before_hit.misses &&
            misses_for(src.stats(), "jetton_wallet") ==
                misses_for(before_hit, "jetton_wallet") &&
            src.stats().tier1_hits == before_hit.tier1_hits + 1 &&
            src.stats().tier2_hits == before_hit.tier2_hits);
}

}  // namespace

int run_celldb_tier2_test() {
  g_fail = 0;
  test_tier_shapes();
  test_nft_code_hash();
  test_nominator_predicate();
  test_jvault_parsers();
  test_jvault_hook();
  test_memo_and_guards();
  test_two_tier_routing();
  test_tier1_authoritative_miss();
  test_account_lookup_states();
  std::printf("CELLDB-TIER2-TEST %s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
  return g_fail == 0 ? 0 : 1;
}

}  // namespace mch
