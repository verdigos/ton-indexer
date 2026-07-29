#include "KvrocksDecodeTest.h"

#include "BuildDriver.h"  // render_value
#include "BuildRuntime.h"
#include "ClassifyDriver.h"
#include "KvrocksDecode.h"

#include "KvrocksState.h"  // the REAL payload writers + row types
#include "convert-utils.h"  // convert::to_raw_address

#include "td/utils/base64.h"
#include "td/utils/logging.h"

#include "common/refint.h"
#include "vm/boc.h"
#include "vm/cells/CellBuilder.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace mch {

namespace {

using kvrocks_state::build_payload;

int g_pass = 0;
int g_fail = 0;

void check(const std::string &name, bool ok, const std::string &detail = {}) {
  if (ok) {
    g_pass++;
    std::printf("PASS %s\n", name.c_str());
  } else {
    g_fail++;
    std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : " -- ", detail.c_str());
  }
}

void check_eq(const std::string &name, const std::string &got, const std::string &want) {
  check(name, got == want, got == want ? "" : "got " + got + ", want " + want);
}

block::StdAddress addr(int wc, char fill) {
  block::StdAddress a;
  a.workchain = wc;
  for (int i = 0; i < 32; i++) {
    a.addr.data()[i] = static_cast<td::uint8>(fill);
  }
  return a;
}

std::string raw(const block::StdAddress &a) {
  return convert::to_raw_address(a);
}

td::Bits256 hash_of(char fill) {
  td::Bits256 h;
  for (int i = 0; i < 32; i++) {
    h.data()[i] = static_cast<td::uint8>(fill);
  }
  return h;
}

// The nominator pool's identifying code hash, as the base64 the writer emits.
td::Bits256 nominator_code_hash() {
  td::Bits256 h;
  auto r = td::base64_decode(td::Slice("mj7BS8CY9rRAZMMFIiyuooAPF92oXuaoGYpwle3hDc8="));
  CHECK(r.is_ok());
  std::string s = r.move_as_ok();
  CHECK(s.size() == 32);
  std::memcpy(h.data(), s.data(), 32);
  return h;
}

const Value *field(const Value &v, const std::string &name) {
  return v.field(name);
}

std::string field_str(const Value &v, const std::string &name) {
  const Value *f = field(v, name);
  return f == nullptr ? "<absent>" : render_value(*f);
}

// --- row builders (values are ours; the ENCODING is the writer's) ------------

kvrocks_state::PreparedJettonWalletRow jetton_wallet_row(bool destroyed = false) {
  kvrocks_state::PreparedJettonWalletRow row{};
  // Deliberately larger than 2^64 so a JSON-number read would lose it: the
  // writer emits int256 as a DECIMAL STRING (§6.1) and the decoder must parse
  // it as one.
  row.balance = td::dec_string_to_int256(td::Slice("123456789012345678901234567890"));
  row.address = addr(0, 0x11);
  row.owner = addr(0, 0x22);
  row.jetton = addr(0, 0x33);
  row.last_transaction_lt = 42;
  row.code_hash = hash_of(0x44);
  row.data_hash = hash_of(0x55);
  row.destroyed = destroyed;
  row.source_mc_seqno = 7;
  return row;
}

kvrocks_state::PreparedLatestAccountStateRow state_row(const block::StdAddress &account,
                                                       std::optional<std::string> data_boc,
                                                       std::optional<td::Bits256> code_hash) {
  kvrocks_state::PreparedLatestAccountStateRow row{};
  row.account = account;
  row.hash = hash_of(0x01);
  row.balance = td::dec_string_to_int256(td::Slice("1000"));
  row.balance_extra_currencies = "{}";
  row.account_status = "active";
  row.timestamp = 100;
  row.last_trans_hash = hash_of(0x02);
  row.last_trans_lt = 5;
  row.code_hash = code_hash;
  row.data_boc = std::move(data_boc);
  row.source_mc_seqno = 7;
  return row;
}

// A cell holding N addresses back to back, optionally after a header of
// `skip_bits` zero bits — the shape both jvault steps parse (addr_std$10 =
// 3 tag bits + 8 workchain bits + 256 address bits).
std::string address_cell_boc(const std::vector<block::StdAddress> &addrs, int skip_bits = 0) {
  vm::CellBuilder cb;
  for (int i = 0; i < skip_bits; i++) {
    cb.store_long(0, 1);
  }
  for (const block::StdAddress &a : addrs) {
    cb.store_long(0b100, 3);  // addr_std$10 + anycast nothing$0
    cb.store_long(a.workchain, 8);
    cb.store_bits(a.addr.cbits(), 256);
  }
  auto r = vm::std_boc_serialize(cb.finalize());
  CHECK(r.is_ok());
  return td::base64_encode(r.move_as_ok().as_slice());
}

}  // namespace

int run_kvrocks_decode_test() {
  g_pass = g_fail = 0;

  // --- jetton_wallet: shape, decimal-string int256, address normalisation ---
  {
    auto row = jetton_wallet_row();
    Value v = decode_kvrocks_payload("jetton_wallet", build_payload(row));
    check("jetton_wallet decodes", !v.is_null());
    check_eq("jetton_wallet.balance (decimal string int256)", field_str(v, "balance"),
             "123456789012345678901234567890");
    check_eq("jetton_wallet.address", field_str(v, "address"), "str:" + raw(row.address));
    check_eq("jetton_wallet.owner", field_str(v, "owner"), "str:" + raw(row.owner));
    check_eq("jetton_wallet.jetton", field_str(v, "jetton"), "str:" + raw(row.jetton));
    // The A/B parity contract: the same account served from the store and from
    // the fixture map must yield the SAME Obj, field for field.
    Value::Fields jw;
    jw.emplace_back("balance", Value::make_int(row.balance));
    jw.emplace_back("address", Value::make_str(raw(row.address)));
    jw.emplace_back("owner", Value::make_str(raw(row.owner)));
    jw.emplace_back("jetton", Value::make_str(raw(row.jetton)));
    Value::Fields acc;
    acc.emplace_back("JettonWallet", Value::make_dict(std::move(jw)));
    std::map<std::string, Value> ifaces;
    ifaces.emplace(raw(row.address), Value::make_dict(std::move(acc)));
    FixtureLookupSource fix(&ifaces);
    Value fixture_v = fix.fetch("jetton_wallet", {Value::make_str(raw(row.address))});
    check_eq("jetton_wallet == fixture source shape", render_value(v), render_value(fixture_v));
  }

  // --- destroyed is honoured, per table ---------------------------------------
  {
    Value v = decode_kvrocks_payload("jetton_wallet", build_payload(jetton_wallet_row(true)));
    check("destroyed:true row reads as absent", v.is_null());
  }

  // --- the address-case hazard (§3.2) -----------------------------------------
  {
    auto row = jetton_wallet_row();
    const std::string upper = raw(row.address);
    std::string lower = upper;
    for (char &c : lower) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    auto id = kvrocks_address_id(lower);
    check("lowercase argument normalises to the writer's id", id.has_value() && *id == upper,
          id ? "got " + *id + ", want " + upper : "got nullopt");
    check_eq("payload key matches the writer's layout",
             kvrocks_payload_key("jetton_wallets", upper),
             "ton-index:v1:jetton_wallets:" + upper + ":payload");
    check("malformed address is a clean miss", !kvrocks_address_id("not-an-address").has_value());
  }

  // --- nft_item / nft_sale / nft_auction / dedust_pool -------------------------
  {
    kvrocks_state::PreparedNftItemRow row{};
    row.address = addr(0, 0x61);
    row.init = true;
    row.index = td::dec_string_to_int256(td::Slice("5"));
    row.collection_address = addr(0, 0x62);
    row.owner_address = addr(0, 0x63);
    row.code_hash = hash_of(0x64);
    row.data_hash = hash_of(0x65);
    row.destroyed = false;
    Value v = decode_kvrocks_payload("nft_item", build_payload(row));
    check_eq("nft_item.address", field_str(v, "address"), "str:" + raw(row.address));
    check_eq("nft_item.init", field_str(v, "init"), "true");
    check_eq("nft_item.index", field_str(v, "index"), "5");
    check_eq("nft_item.collection_address", field_str(v, "collection_address"),
             "str:" + raw(*row.collection_address));
    check_eq("nft_item.owner_address", field_str(v, "owner_address"),
             "str:" + raw(*row.owner_address));
    // Absent content is still Null — that is the same rejection Python takes for
    // a content-less item, not the bug below.
    check_eq("nft_item.content absent stays null", field_str(v, "content"), "null");

    // §6.1a: content is the field whose Null DROPPED nft_transfer rows, because
    // get_nft_data faults on a non-Dict. The writer embeds it as raw JSON, so
    // this asserts the decoder reads the OBJECT the writer emits, not a string.
    row.content = std::string(
        R"({"uri":"https://nft.fragment.com/number/888.json","name":"n","x":7,"nested":{"a":1}})");
    Value with = decode_kvrocks_payload("nft_item", build_payload(row));
    const Value *content = with.field("content");
    check("nft_item.content decodes to a Dict",
          content != nullptr && content->t == VType::Dict,
          content == nullptr ? "absent" : "type " + std::to_string(static_cast<int>(content->t)));
    if (content != nullptr && content->t == VType::Dict) {
      // The one key with observable behaviour: the fragment.com branch of
      // get_nft_data reads it, and tier 1 carries it as a plain string too.
      check_eq("nft_item.content.uri", field_str(*content, "uri"),
               "str:https://nft.fragment.com/number/888.json");
      check_eq("nft_item.content number value flattens to Str", field_str(*content, "x"), "str:7");
      // Kept with a null value rather than dropped, so a nested-metadata payload
      // is visible instead of silently thinned.
      check_eq("nft_item.content nested object keeps its key", field_str(*content, "nested"),
               "null");
    }
  }
  {
    kvrocks_state::PreparedGetgemsSaleRow row{};
    row.address = addr(0, 0x71);
    row.marketplace_address = addr(0, 0x72);
    row.nft_address = addr(0, 0x73);
    row.nft_owner_address = addr(0, 0x74);
    row.full_price = td::dec_string_to_int256(td::Slice("1350000000"));
    row.marketplace_fee = td::dec_string_to_int256(td::Slice("1"));
    row.marketplace_fee_address = addr(0, 0x75);
    row.royalty_address = addr(0, 0x76);
    row.royalty_amount = td::dec_string_to_int256(td::Slice("2"));
    row.code_hash = hash_of(0x77);
    row.data_hash = hash_of(0x78);
    Value v = decode_kvrocks_payload("nft_sale", build_payload(row));
    check_eq("nft_sale.marketplace_address", field_str(v, "marketplace_address"),
             "str:" + raw(row.marketplace_address));
    check_eq("nft_sale.nft_address", field_str(v, "nft_address"), "str:" + raw(row.nft_address));
    check_eq("nft_sale.full_price", field_str(v, "full_price"), "1350000000");
    check_eq("nft_sale.nft_owner_address", field_str(v, "nft_owner_address"),
             "str:" + raw(*row.nft_owner_address));
  }
  {
    kvrocks_state::PreparedGetgemsAuctionRow row{};
    row.address = addr(0, 0x81);
    row.mp_addr = addr(0, 0x82);
    row.nft_addr = addr(0, 0x83);
    row.nft_owner = addr(0, 0x84);
    row.last_bid = td::dec_string_to_int256(td::Slice("999"));
    row.max_bid = td::dec_string_to_int256(td::Slice("0"));
    row.min_bid = td::dec_string_to_int256(td::Slice("0"));
    row.mp_fee_addr = addr(0, 0x85);
    row.royalty_fee_addr = addr(0, 0x86);
    row.code_hash = hash_of(0x87);
    row.data_hash = hash_of(0x88);
    Value v = decode_kvrocks_payload("nft_auction", build_payload(row));
    check_eq("nft_auction.mp_addr", field_str(v, "mp_addr"), "str:" + raw(row.mp_addr));
    check_eq("nft_auction.nft_addr", field_str(v, "nft_addr"), "str:" + raw(row.nft_addr));
    check_eq("nft_auction.last_bid", field_str(v, "last_bid"), "999");
    check_eq("nft_auction.nft_owner", field_str(v, "nft_owner"), "str:" + raw(*row.nft_owner));
  }
  {
    kvrocks_state::PreparedDedustPoolRow row{};
    row.address = addr(0, 0x91);
    row.asset_1 = std::nullopt;  // the TON side
    row.asset_2 = addr(0, 0x92);
    row.reserve_1 = td::dec_string_to_int256(td::Slice("0"));
    row.reserve_2 = td::dec_string_to_int256(td::Slice("0"));
    row.pool_type = "volatile";
    row.dex = "dedust";
    row.fee = 0.0;
    row.code_hash = hash_of(0x93);
    row.data_hash = hash_of(0x94);
    Value v = decode_kvrocks_payload("dedust_pool", build_payload(row));
    const Value *assets = field(v, "assets");
    bool shape = assets != nullptr && assets->t == VType::List && assets->items != nullptr &&
                 assets->items->size() == 2;
    check("dedust_pool.assets is a 2-element list", shape);
    if (shape) {
      check_eq("dedust_pool asset_1 is TON", render_value((*assets->items)[0]),
               render_value([] {
                 Value::Fields f;
                 f.emplace_back("is_ton", Value::make_bool(true));
                 f.emplace_back("address", Value::null());
                 return Value::make_obj(std::move(f));
               }()));
      check_eq("dedust_pool asset_2 address", field_str((*assets->items)[1], "address"),
               "str:" + raw(*row.asset_2));
    }
  }

  // --- nominator_pool: code-hash gate, and NO destroyed rule on states ---------
  {
    auto pool = addr(0, 0xA1);
    std::string payload = build_payload(state_row(pool, std::nullopt, nominator_code_hash()));
    check("latest_account_states carries no `destroyed` field",
          payload.find("\"destroyed\"") == std::string::npos);
    Value v = decode_kvrocks_payload("nominator_pool", payload);
    check("nominator_pool resolves on the known code hash", !v.is_null());
    check_eq("nominator_pool.address", field_str(v, "address"), "str:" + raw(pool));
    Value other =
        decode_kvrocks_payload("nominator_pool", build_payload(state_row(pool, std::nullopt,
                                                                        hash_of(0x0F))));
    check("nominator_pool rejects a different code hash", other.is_null());
  }

  // --- jvault: the 3-round chain through the batch resolver (§6.2/§6.5) -------
  {
    auto stake_wallet = addr(0, 0x31);
    auto pool = addr(0, 0x32);
    auto minter = addr(0, 0x33);
    auto lock_wallet = addr(0, 0x34);
    auto jetton = addr(0, 0x35);

    auto wallet_row = jetton_wallet_row();
    wallet_row.address = lock_wallet;
    wallet_row.jetton = jetton;

    std::map<std::string, std::string> store;
    store[kvrocks_payload_key("latest_account_states", raw(stake_wallet))] =
        build_payload(state_row(stake_wallet, address_cell_boc({pool, minter}), std::nullopt));
    // The pool's data cell: 1+32 header bits, then admin / creator / lock wallet.
    store[kvrocks_payload_key("latest_account_states", raw(pool))] = build_payload(
        state_row(pool, address_cell_boc({addr(0, 0x36), addr(0, 0x37), lock_wallet}, 33),
                  std::nullopt));
    store[kvrocks_payload_key("jetton_wallets", raw(lock_wallet))] = build_payload(wallet_row);

    int rounds = 0;
    PayloadFetch fetch = [&store, &rounds](const std::vector<std::string> &keys) {
      rounds++;
      std::map<std::string, std::string> out;
      for (const std::string &k : keys) {
        auto it = store.find(k);
        out.emplace(k, it != store.end() ? it->second : std::string());  // absent -> ""
      }
      return out;
    };

    LookupWants wanted;
    std::vector<Value> args{Value::make_str(raw(stake_wallet))};
    wanted.emplace(lookup_key("jvault_assets", args), std::make_pair("jvault_assets", args));

    auto out = kvrocks_resolve_batch(wanted, fetch);
    auto it = out.find(lookup_key("jvault_assets", args));
    check("jvault: the batch answers every requested key", it != out.end());
    if (it != out.end()) {
      check_eq("jvault.staking_pool", field_str(it->second, "staking_pool"), raw(pool));
      check_eq("jvault.asset", field_str(it->second, "asset"), "asset:" + raw(jetton));
      check_eq("jvault.jvault_asset", field_str(it->second, "jvault_asset"),
               "asset:" + raw(minter));
    }
    check("jvault: 3 chained fetches, no more", rounds == 3, "rounds=" + std::to_string(rounds));

    // Pool state absent => the PARTIAL record (jvault.py:58-59), not a failure.
    store.erase(kvrocks_payload_key("latest_account_states", raw(pool)));
    auto partial = kvrocks_resolve_batch(wanted, fetch);
    auto pit = partial.find(lookup_key("jvault_assets", args));
    check("jvault partial: record survives", pit != partial.end() && !pit->second.is_null());
    if (pit != partial.end() && !pit->second.is_null()) {
      check_eq("jvault partial: staking_pool kept", field_str(pit->second, "staking_pool"),
               raw(pool));
      check_eq("jvault partial: asset null", field_str(pit->second, "asset"), "null");
      check_eq("jvault partial: jvault_asset kept", field_str(pit->second, "jvault_asset"),
               "asset:" + raw(minter));
    }

    // Stake wallet state absent => the whole record is null.
    store.clear();
    auto none = kvrocks_resolve_batch(wanted, fetch);
    auto nit = none.find(lookup_key("jvault_assets", args));
    check("jvault: unresolvable stake wallet yields a null record",
          nit != none.end() && nit->second.is_null());
  }

  std::printf("\n%d/%d kvrocks decode checks passed\n", g_pass, g_pass + g_fail);
  return g_fail == 0 ? 0 : 1;
}

}  // namespace mch
