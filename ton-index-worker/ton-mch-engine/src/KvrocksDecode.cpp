#include "KvrocksDecode.h"

#include "parse/PSlice.h"  // load_address_py

#include "td/utils/JsonBuilder.h"
#include "td/utils/Slice.h"
#include "td/utils/base64.h"

#include "common/refint.h"
#include "vm/boc.h"
#include "vm/cellslice.h"

#include <string>
#include <vector>

namespace mch {

namespace {

using Json = td::JsonValue;
using JType = td::JsonValue::Type;

const Json *jfield(const Json &e, td::Slice name) {
  if (e.type() != JType::Object) {
    return nullptr;
  }
  for (const auto &kv : e.get_object().field_values_) {
    if (kv.first == name) {
      return &kv.second;
    }
  }
  return nullptr;
}

// A decoded payload plus the buffer its slices point into: td::json_decode
// parses IN PLACE, so the buffer has to outlive every read of the tree.
struct Decoded {
  std::string buf;
  Json root;
  bool ok = false;
};

Decoded decode_object(const std::string &payload) {
  Decoded d;
  if (payload.empty()) {
    return d;  // absent row: MGET answers nil
  }
  d.buf = payload;
  auto r_root = td::json_decode(td::MutableSlice(d.buf));
  if (r_root.is_error()) {
    return d;
  }
  d.root = r_root.move_as_ok();
  d.ok = d.root.type() == JType::Object;
  return d;
}

// `"destroyed": true` == absent (interface_repository.py:126-127). Tables whose
// builder emits no such field simply never trip this.
bool destroyed(const Json &root) {
  const Json *f = jfield(root, "destroyed");
  return f != nullptr && f->type() == JType::Boolean && f->get_boolean();
}

// An address field: raw "wc:HEX" string -> normalized Str, JSON null -> Value
// null. Anything else (wrong type, unparseable) is also null — a payload the
// writer could not have produced degrades rather than faults.
Value addr_field(const Json &root, td::Slice name) {
  const Json *f = jfield(root, name);
  if (f == nullptr || f->type() != JType::String) {
    return Value::null();
  }
  auto norm = normalize_raw_address(f->get_string().str());
  return norm ? Value::make_str(*norm) : Value::null();
}

// int256 arrives as a DECIMAL STRING (json_put_int256 -> to_dec_string), so this
// is a string read plus dec_string_to_int256 — never a JSON number read.
Value int_field(const Json &root, td::Slice name) {
  const Json *f = jfield(root, name);
  if (f == nullptr || f->type() != JType::String) {
    return Value::null();
  }
  td::RefInt256 v = td::dec_string_to_int256(f->get_string());
  return v.is_null() ? Value::null() : Value::make_int(v);
}

Value bool_field(const Json &root, td::Slice name) {
  const Json *f = jfield(root, name);
  if (f == nullptr || f->type() != JType::Boolean) {
    return Value::null();
  }
  return Value::make_bool(f->get_boolean());
}

std::string str_field(const Json &root, td::Slice name) {
  const Json *f = jfield(root, name);
  return f != nullptr && f->type() == JType::String ? f->get_string().str() : std::string();
}

// nft_item.content — the metadata object, which the writer embeds as RAW JSON
// (json_put_optional_raw_json, KvrocksState.cpp), so it arrives here as a nested
// object rather than a string. It MUST become a Dict: get_nft_data faults on a
// non-Dict and the whole nft_transfer row is dropped, which is the tier-2 half
// of the action loss in em2-tier2-design §6.1a — landing tier 2 without this
// would have left counterparty NFTs, the very case tier 2 exists for, still
// dropping rows.
//
// Values are flattened to Str, matching what tier 1 can produce out of
// NFTItemDataV2's flat map<string,string>; the two tiers must agree, and the
// only observable reads are the top-level `uri` (a string on both) and a
// whole-dict passthrough no fill or spec ever reads. A nested object or array
// value keeps its key with a null value rather than vanishing, so a shape
// surprise is visible instead of silent.
Value json_to_str_value(const Json &v) {
  switch (v.type()) {
    case JType::String: return Value::make_str(v.get_string().str());
    case JType::Number: return Value::make_str(v.get_number().str());
    case JType::Boolean: return Value::make_bool(v.get_boolean());
    default: return Value::null();  // Null / Object / Array
  }
}

Value nft_content(const Json &root) {
  const Json *f = jfield(root, "content");
  if (f == nullptr || f->type() != JType::Object) {
    return Value::null();  // absent content == the rejection Python also takes
  }
  Value::Fields out;
  for (const auto &kv : f->get_object().field_values_) {
    out.emplace_back(kv.first.str(), json_to_str_value(kv.second));
  }
  return Value::make_dict(std::move(out));
}

// A dedust pool asset slot: {is_ton, address}, TON when the address is absent —
// the two fields HostDedust reads (ParsedBlockLookupSource.cpp:117-127).
Value dedust_asset(const Json &root, td::Slice name) {
  Value address = addr_field(root, name);
  Value::Fields f;
  f.emplace_back("is_ton", Value::make_bool(address.is_null()));
  f.emplace_back("address", std::move(address));
  return Value::make_obj(std::move(f));
}

// The nominator pool's identifying code hash (§6.1): the payload is only
// null-checked by the spec (nominator.mch:34-35), so any non-null Obj serves.
constexpr td::Slice kNominatorCodeHash = "mj7BS8CY9rRAZMMFIiyuooAPF92oXuaoGYpwle3hDc8=";

// The account-state data cell as a CellSlice, or an error. Shared by both jvault
// steps; mirrors the fixture source's b64 -> BOC -> load_cell_slice_special.
td::Result<vm::CellSlice> state_data_slice(const std::string &payload) {
  Decoded d = decode_object(payload);
  if (!d.ok) {
    return td::Status::Error("no payload");
  }
  const std::string boc = str_field(d.root, "data_boc");
  if (boc.empty()) {
    return td::Status::Error("no data_boc");
  }
  TRY_RESULT(raw, td::base64_decode(td::Slice(boc)));
  TRY_RESULT(cell, vm::std_boc_deserialize(raw));
  if (cell.is_null()) {
    return td::Status::Error("empty boc");
  }
  bool special = false;
  try {
    return vm::load_cell_slice_special(cell, special);
  } catch (...) {
    return td::Status::Error("cell load");
  }
}

}  // namespace

std::string kvrocks_payload_key(const std::string &table, const std::string &id) {
  return "ton-index:v1:" + table + ":" + id + ":payload";
}

std::optional<std::string> kvrocks_table_for_kind(const std::string &kind) {
  if (kind == "jetton_wallet") return "jetton_wallets";
  if (kind == "nft_item") return "nft_items";
  if (kind == "nft_sale") return "getgems_nft_sales";
  if (kind == "nft_auction") return "getgems_nft_auctions";
  if (kind == "dedust_pool") return "dex_pools";
  if (kind == "nominator_pool") return "latest_account_states";
  return std::nullopt;  // jvault_assets: a chain, not a table (§6.2)
}

std::optional<std::string> kvrocks_address_id(const std::string &raw_address) {
  return normalize_raw_address(raw_address);
}

Value decode_kvrocks_payload(const std::string &kind, const std::string &payload) {
  Decoded d = decode_object(payload);
  if (!d.ok || destroyed(d.root)) {
    return Value::null();
  }
  const Json &r = d.root;

  if (kind == "jetton_wallet") {
    Value::Fields f;
    f.emplace_back("balance", int_field(r, "balance"));
    f.emplace_back("address", addr_field(r, "address"));
    f.emplace_back("owner", addr_field(r, "owner"));
    f.emplace_back("jetton", addr_field(r, "jetton"));
    return Value::make_obj(std::move(f));
  }
  if (kind == "nft_item") {
    Value::Fields f;
    f.emplace_back("address", addr_field(r, "address"));
    f.emplace_back("init", bool_field(r, "init"));
    f.emplace_back("index", int_field(r, "index"));
    f.emplace_back("collection_address", addr_field(r, "collection_address"));
    f.emplace_back("owner_address", addr_field(r, "owner_address"));
    f.emplace_back("content", nft_content(r));
    return Value::make_obj(std::move(f));
  }
  if (kind == "nft_sale") {
    // ONE shape covers V1 and V4: the V4 rows are folded into the same
    // getgems_nft_sales table with the same PreparedGetgemsSaleRow payload
    // (KvrocksState.cpp:2183-2184,2227,2877-2878) — the two tier-1 variants
    // differ only on the ParsedBlock side.
    Value::Fields f;
    f.emplace_back("marketplace_address", addr_field(r, "marketplace_address"));
    f.emplace_back("nft_address", addr_field(r, "nft_address"));
    f.emplace_back("full_price", int_field(r, "full_price"));
    f.emplace_back("nft_owner_address", addr_field(r, "nft_owner_address"));
    return Value::make_obj(std::move(f));
  }
  if (kind == "nft_auction") {
    Value::Fields f;
    f.emplace_back("mp_addr", addr_field(r, "mp_addr"));
    f.emplace_back("nft_addr", addr_field(r, "nft_addr"));
    f.emplace_back("last_bid", int_field(r, "last_bid"));
    f.emplace_back("nft_owner", addr_field(r, "nft_owner"));
    return Value::make_obj(std::move(f));
  }
  if (kind == "dedust_pool") {
    std::vector<Value> assets;
    assets.push_back(dedust_asset(r, "asset_1"));
    assets.push_back(dedust_asset(r, "asset_2"));
    Value::Fields f;
    f.emplace_back("assets", Value::make_list(std::move(assets)));
    return Value::make_obj(std::move(f));
  }
  if (kind == "nominator_pool") {
    // latest_account_states carries no `destroyed` field, so the check above is
    // a no-op here by construction — same as Python, which applies no destroyed
    // rule to states (interface_repository.py:868-870).
    if (str_field(r, "code_hash") != kNominatorCodeHash) {
      return Value::null();
    }
    Value::Fields f;
    f.emplace_back("address", addr_field(r, "account"));
    return Value::make_obj(std::move(f));
  }
  return Value::null();
}

std::optional<JvaultStake> kvrocks_jvault_stake(const std::string &state_payload) {
  auto r_cs = state_data_slice(state_payload);
  if (r_cs.is_error()) {
    return std::nullopt;
  }
  vm::CellSlice cs = r_cs.move_as_ok();
  auto r_pool = load_address_py(cs);
  if (r_pool.is_error()) {
    return std::nullopt;
  }
  Value pool = r_pool.move_as_ok();
  auto r_minter = load_address_py(cs);
  if (r_minter.is_error()) {
    return std::nullopt;
  }
  Value minter = r_minter.move_as_ok();
  // Python does staking_pool.to_str(...) unconditionally: an addr_none pool
  // AttributeErrors into the outer except, i.e. the whole record is null.
  if (pool.t != VType::Account || pool.addr_none) {
    return std::nullopt;
  }
  JvaultStake out;
  out.staking_pool = pool;
  out.jvault_asset = Value::null();
  if (minter.t == VType::Account && !minter.addr_none) {
    auto norm = normalize_raw_address(minter.str);
    out.jvault_asset = Value::make_asset_jetton(norm ? *norm : minter.str);
  }
  return out;
}

std::optional<std::string> kvrocks_jvault_lock_wallet(const std::string &pool_state_payload) {
  // derive_accounts_from_stake_pool (address_selectors.py:128-139): skip the
  // 1+32 header bits, then admin / creator / lock_wallet.
  auto r_cs = state_data_slice(pool_state_payload);
  if (r_cs.is_error()) {
    return std::nullopt;
  }
  vm::CellSlice cs = r_cs.move_as_ok();
  if (!cs.advance(1 + 32)) {
    return std::nullopt;
  }
  for (int i = 0; i < 2; i++) {  // admin_address, creator_address
    if (load_address_py(cs).is_error()) {
      return std::nullopt;
    }
  }
  auto r_lock = load_address_py(cs);
  if (r_lock.is_error()) {
    return std::nullopt;
  }
  Value lock = r_lock.move_as_ok();
  if (lock.t != VType::Account || lock.addr_none) {
    return std::nullopt;
  }
  return normalize_raw_address(lock.str);
}

Value kvrocks_jvault_record(const JvaultStake &stake, Value asset) {
  Value::Fields f;
  f.emplace_back("staking_pool", stake.staking_pool);
  f.emplace_back("asset", std::move(asset));
  f.emplace_back("jvault_asset", stake.jvault_asset);
  return Value::make_obj(std::move(f));
}

std::map<std::string, Value> kvrocks_resolve_batch(const LookupWants &wanted,
                                                   const PayloadFetch &fetch) {
  std::map<std::string, Value> out;
  // What each wanted key needs from round A: the store key it reads, and — for
  // jvault — the chain state carried across the later rounds.
  struct Pending {
    std::string kind;
    std::string key;  // round-A payload key ("" == unresolvable address)
  };
  std::map<std::string, Pending> pending;  // lookup_key -> what to fetch
  std::vector<std::string> keys_a;

  for (const auto &[lookup_key_str, ka] : wanted) {
    const std::string &kind = ka.first;
    const std::vector<Value> &args = ka.second;
    if (args.size() != 1 || args[0].t != VType::Str) {
      out.emplace(lookup_key_str, Value::null());  // not an address lookup
      continue;
    }
    auto id = kvrocks_address_id(args[0].str);
    if (!id) {
      out.emplace(lookup_key_str, Value::null());  // malformed: clean miss
      continue;
    }
    // jvault's first read is a latest_account_states row, so it rides the same
    // fetch as nominator_pool rather than costing a round of its own.
    const std::string table =
        kind == "jvault_assets" ? "latest_account_states"
                                : kvrocks_table_for_kind(kind).value_or(std::string());
    if (table.empty()) {
      out.emplace(lookup_key_str, Value::null());  // unknown kind
      continue;
    }
    Pending p{kind, kvrocks_payload_key(table, *id)};
    keys_a.push_back(p.key);
    pending.emplace(lookup_key_str, std::move(p));
  }
  if (pending.empty()) {
    return out;
  }

  std::map<std::string, std::string> payloads = fetch(keys_a);
  auto payload_of = [&payloads](const std::string &key) {
    auto it = payloads.find(key);
    return it != payloads.end() ? it->second : std::string();
  };

  // Round A results, plus the jvault stakes that resolved a pool.
  struct Chain {
    JvaultStake stake;
    std::string pool_key;    // round B
    std::string wallet_key;  // round C
  };
  std::map<std::string, Chain> chains;
  std::vector<std::string> keys_b;
  for (const auto &[lookup_key_str, p] : pending) {
    if (p.kind != "jvault_assets") {
      out.emplace(lookup_key_str, decode_kvrocks_payload(p.kind, payload_of(p.key)));
      continue;
    }
    auto stake = kvrocks_jvault_stake(payload_of(p.key));
    if (!stake) {
      out.emplace(lookup_key_str, Value::null());
      continue;
    }
    auto pool_id = kvrocks_address_id(stake->staking_pool.str);
    if (!pool_id) {
      out.emplace(lookup_key_str, Value::null());
      continue;
    }
    Chain c{*stake, kvrocks_payload_key("latest_account_states", *pool_id), {}};
    keys_b.push_back(c.pool_key);
    chains.emplace(lookup_key_str, std::move(c));
  }
  if (chains.empty()) {
    return out;
  }

  payloads = fetch(keys_b);
  std::vector<std::string> keys_c;
  for (auto &[lookup_key_str, c] : chains) {
    auto lock = kvrocks_jvault_lock_wallet(payload_of(c.pool_key));
    if (!lock) {
      // Pool resolved, its state absent or unparseable: the PARTIAL record
      // (jvault.py:58-59), not a failure.
      out.emplace(lookup_key_str, kvrocks_jvault_record(c.stake, Value::null()));
      continue;
    }
    c.wallet_key = kvrocks_payload_key("jetton_wallets", *lock);
    keys_c.push_back(c.wallet_key);
  }
  if (keys_c.empty()) {
    return out;
  }

  payloads = fetch(keys_c);
  for (const auto &[lookup_key_str, c] : chains) {
    if (c.wallet_key.empty()) {
      continue;  // already answered above
    }
    Value asset = kvrocks_jvault_asset_from_wallet(payload_of(c.wallet_key));
    // A missing wallet is Python's get_jetton_wallet -> None -> .jetton
    // AttributeError -> the whole record null, NOT the partial record.
    out.emplace(lookup_key_str,
                asset.is_null() ? Value::null() : kvrocks_jvault_record(c.stake, asset));
  }
  return out;
}

Value kvrocks_jvault_asset_from_wallet(const std::string &wallet_payload) {
  Value wallet = decode_kvrocks_payload("jetton_wallet", wallet_payload);
  if (wallet.is_null()) {
    return Value::null();
  }
  const Value *jetton = wallet.field("jetton");
  if (jetton == nullptr || jetton->t != VType::Str) {
    return Value::null();
  }
  return Value::make_asset_jetton(jetton->str);
}

}  // namespace mch
