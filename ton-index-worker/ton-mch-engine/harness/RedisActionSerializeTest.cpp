// --actions-msgpack-test: action-row envelope and payload parity checks.
#include "RedisActionSerializeTest.h"

#include "ActionBuild.h"
#include "ActionPayload.h"
#include "RedisActionSerialize.h"
#include "SchemaTxTest.h"
#include "Value.h"
#include "emu/EmuTypes.h"
#include "td/utils/base64.h"

#include <msgpack.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace mch {
namespace {

int g_fail = 0;

void check(const std::string &name, bool ok) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
  if (!ok) g_fail++;
}

class Decoded {
 public:
  explicit Decoded(std::string bytes) : bytes_(std::move(bytes)) {
    try {
      handle_ = msgpack::unpack(bytes_.data(), bytes_.size());
      ok_ = true;
    } catch (const std::exception &) {
      ok_ = false;
    }
  }

  bool ok() const { return ok_; }
  const msgpack::object &root() const { return handle_.get(); }

 private:
  std::string bytes_;
  msgpack::object_handle handle_;
  bool ok_{false};
};

const msgpack::object *at(const msgpack::object &map, const char *key) {
  if (map.type != msgpack::type::MAP) return nullptr;
  const auto n = std::strlen(key);
  for (std::uint32_t i = 0; i < map.via.map.size; i++) {
    const auto &candidate = map.via.map.ptr[i].key;
    if (candidate.type == msgpack::type::STR && candidate.via.str.size == n &&
        std::memcmp(candidate.via.str.ptr, key, n) == 0) {
      return &map.via.map.ptr[i].val;
    }
  }
  return nullptr;
}

const msgpack::object *at_id(const msgpack::object &map, std::uint8_t id) {
  if (map.type != msgpack::type::MAP) return nullptr;
  for (std::uint32_t i = 0; i < map.via.map.size; ++i) {
    const auto &entry = map.via.map.ptr[i];
    if (entry.key.type == msgpack::type::POSITIVE_INTEGER && entry.key.via.u64 == id)
      return &entry.val;
  }
  return nullptr;
}

const msgpack::object *elem(const msgpack::object *array, std::uint32_t index) {
  if (array == nullptr || array->type != msgpack::type::ARRAY || index >= array->via.array.size) {
    return nullptr;
  }
  return &array->via.array.ptr[index];
}

const msgpack::object *first_row(const Decoded &decoded) {
  return elem(&decoded.root(), 0);
}

bool absent(const msgpack::object &map, const char *key) { return at(map, key) == nullptr; }

bool is_str(const msgpack::object *value, const std::string &want) {
  return value != nullptr && value->type == msgpack::type::STR && value->via.str.size == want.size() &&
         std::memcmp(value->via.str.ptr, want.data(), want.size()) == 0;
}

bool is_hash_array(const msgpack::object *value, const std::vector<std::string> &want) {
  if (value == nullptr || value->type != msgpack::type::ARRAY || value->via.array.size != want.size()) return false;
  for (std::size_t i = 0; i < want.size(); i++) {
    if (!is_str(elem(value, static_cast<std::uint32_t>(i)), want[i])) return false;
  }
  return true;
}

bool is_uint(const msgpack::object *value, std::uint64_t want) {
  return value != nullptr && value->type == msgpack::type::POSITIVE_INTEGER && value->via.u64 == want;
}

bool is_bool(const msgpack::object *value, bool want) {
  return value != nullptr && value->type == msgpack::type::BOOLEAN && value->via.boolean == want;
}

bool is_binary(const msgpack::object *value, const std::string &want) {
  return value != nullptr && value->type == msgpack::type::BIN && value->via.bin.size == want.size() &&
         std::memcmp(value->via.bin.ptr, want.data(), want.size()) == 0;
}

std::string binary_string(const msgpack::object *value) {
  if (value == nullptr || value->type != msgpack::type::BIN) return {};
  return std::string(value->via.bin.ptr, value->via.bin.size);
}

const std::string kAddrA = "0:" + std::string(64, 'A');
const std::string kAddrB = "0:" + std::string(64, 'B');
const std::string kAddrC = "0:" + std::string(64, 'C');
const std::string kTraceKey = "6JLNRxi7yQKUYYSGxSt8SVLZKvC3vHMqBLHqOKTqDGA=";

EmuTraceView make_view(std::vector<EmuFinality> finalities) {
  EmuTraceView view;
  view.trace_id = kTraceKey;
  for (EmuFinality finality : finalities) {
    EmuTxRef node;
    node.finality = finality;
    view.nodes.push_back(std::move(node));
  }
  return view;
}

Action base_action() {
  Action action;
  action.action_id = "aQpDaKBb2mZmBBHkxTZ9xxJgqhZP4mZBTvUAn1KoLpA=";
  action.type = "ton_transfer";
  action.success = true;
  action.start_lt = 58291746000001;
  action.end_lt = 58291746000003;
  action.start_utime = 1753900000;
  action.end_utime = 1753900002;
  action.tx_hashes = {"txA", "txB"};
  action.accounts = {kAddrA, kAddrB};
  return action;
}

void test_empty_actions() {
  Decoded decoded(serialize_redis_actions({}, make_view({EmuFinality::finalized})));
  check("empty/array", decoded.ok() && decoded.root().type == msgpack::type::ARRAY &&
                           decoded.root().via.array.size == 0);
}

void test_fixed_envelope() {
  const Action action = base_action();
  Decoded decoded(serialize_redis_actions({action}, make_view({EmuFinality::finalized})));
  const auto *row = first_row(decoded);
  check("fixed/one_row", decoded.ok() && row != nullptr);
  if (row == nullptr) return;

  check("fixed/action_id", is_str(at(*row, "action_id"), action.action_id));
  check("fixed/type", is_str(at(*row, "type"), action.type));
  check("fixed/success_bool", is_bool(at(*row, "success"), true));
  check("fixed/finality_int", is_uint(at(*row, "finality"), 2));
  check("fixed/trace_external_hash_norm", is_str(at(*row, "trace_external_hash_norm"), kTraceKey));
  check("fixed/tx_hashes", at(*row, "tx_hashes") != nullptr);
  check("fixed/accounts", at(*row, "accounts") != nullptr);
  check("fixed/payload_binary", at(*row, "payload") != nullptr &&
                                   at(*row, "payload")->type == msgpack::type::BIN);
  check("fixed/exact_key_count", row->via.map.size == 12);
}

void test_columns_and_payload() {
  Action action = base_action();
  action.type = "jetton_transfer";
  action.source = Value::make_str(kAddrA);
  action.destination = Value::make_str(kAddrB);
  action.asset = Value::make_str(kAddrA);
  action.amount = Value::make_amount(td::make_refint(123456));
  action.opcode = Value::make_int64(0x0f8a7ea5);
  action.data = Value::make_dict({
      {"query_id", Value::make_int(td::string_to_int256(std::string("18446744073709551615")))},
      {"comment", Value::make_str("hello")},
      {"comment_encrypted", Value::make_bool(false)},
  });

  Decoded row_bytes(serialize_redis_actions({action}, make_view({EmuFinality::confirmed})));
  const auto *row = first_row(row_bytes);
  check("columns/row", row_bytes.ok() && row != nullptr);
  if (row == nullptr) return;
  check("columns/source", is_str(at(*row, "source"), kAddrA));
  check("columns/destination", is_str(at(*row, "destination"), kAddrB));
  check("columns/asset", is_str(at(*row, "asset"), kAddrA));
  check("columns/amount_decimal", is_str(at(*row, "amount"), "123456"));
  check("columns/opcode", is_uint(at(*row, "opcode"), 0x0f8a7ea5));
  check("columns/no_data_column", absent(*row, "data"));

  const std::string wire_payload = binary_string(at(*row, "payload"));
  check("columns/payload_codec", !wire_payload.empty() && wire_payload.front() == '\x01');
  Decoded payload(wire_payload.size() > 1 ? wire_payload.substr(1) : std::string{});
  check("columns/payload_decodes", payload.ok() && payload.root().type == msgpack::type::MAP);
  check("columns/flat_map", payload.root().type == msgpack::type::MAP && payload.root().via.map.size == 3);
  check("columns/query_id_uint", is_uint(at_id(payload.root(), id_of("query_id")), UINT64_MAX));
  check("columns/comment", is_str(at_id(payload.root(), id_of("comment")), "hello"));
  check("columns/comment_encrypted", is_bool(at_id(payload.root(), id_of("comment_encrypted")), false));
}

void test_finality_and_child_fields() {
  Action action = base_action();
  action.ancestor_type = {"jvault_stake", "teleitem_start_auction"};
  Decoded decoded(serialize_redis_actions(
      {action}, make_view({EmuFinality::finalized, EmuFinality::emulated, EmuFinality::confirmed})));
  const auto *row = first_row(decoded);
  check("child/decodes", decoded.ok() && row != nullptr);
  if (row == nullptr) return;
  check("child/ancestor_type", elem(at(*row, "ancestor_type"), 0) != nullptr &&
                                  is_str(elem(at(*row, "ancestor_type"), 0), "jvault_stake"));
  check("child/finality_min", is_uint(at(*row, "finality"), 0));
}

void test_positional_legs() {
  auto payload = [](bool first, bool second) {
    Block block;
    block.btype = "dedust_deposit_liquidity_partial";
    Value::Fields fields{{"dex", Value::make_str("dedust")}};
    auto add_leg = [&](int slot, const std::string &asset, std::int64_t amount) {
      const auto suffix = std::to_string(slot);
      fields.emplace_back("asset_" + suffix, Value::make_account_raw(asset));
      fields.emplace_back("amount_" + suffix, Value::make_int64(amount));
      fields.emplace_back("target_asset_" + suffix, Value::make_account_raw(kAddrB));
      fields.emplace_back("target_amount_" + suffix, Value::make_int64(amount + 1));
      fields.emplace_back("user_jetton_wallet_" + suffix, Value::make_account_raw(kAddrC));
    };
    if (first) add_leg(1, kAddrA, 123);
    if (second) add_leg(2, kAddrA, 456);
    block.data = Value::make_dict(std::move(fields));
    Action action;
    check("legs/build", build_action(&block, action));
    return encode_action_payload(action);
  };

  const auto one_sided = payload(false, true);
  Decoded first_empty(one_sided.substr(1));
  const auto *first_data = &first_empty.root();
  const auto *first_legs = at_id(*first_data, id_of("legs"));
  const auto *slot_1 = elem(first_legs, 0);
  const auto *slot_2 = elem(first_legs, 1);
  check("legs/first_empty", one_sided.front() == '\x01' && first_empty.ok() &&
                               first_legs != nullptr && first_legs->type == msgpack::type::ARRAY &&
                               first_legs->via.array.size == 2 &&
                               is_str(at_id(*first_data, id_of("protocol")), "dedust") &&
                               slot_1 != nullptr && slot_1->type == msgpack::type::NIL &&
                               slot_2 != nullptr && slot_2->type == msgpack::type::MAP &&
                               slot_2->via.map.size == 5 &&
                               at_id(*slot_2, id_of("asset"))->type == msgpack::type::BIN &&
                               is_uint(at_id(*slot_2, id_of("amount")), 456) &&
                               at_id(*slot_2, id_of("target_asset"))->type == msgpack::type::BIN &&
                               is_uint(at_id(*slot_2, id_of("target_amount")), 457) &&
                               at_id(*slot_2, id_of("user_jetton_wallet"))->type == msgpack::type::BIN);

  const auto trailing_empty = payload(true, false);
  Decoded second_empty(trailing_empty.substr(1));
  const auto *second_legs = at_id(second_empty.root(), id_of("legs"));
  const auto *only_leg = elem(second_legs, 0);
  check("legs/trailing_empty", second_empty.ok() && second_legs != nullptr &&
                                  second_legs->type == msgpack::type::ARRAY &&
                                  second_legs->via.array.size == 1 && only_leg != nullptr &&
                                  is_uint(at_id(*only_leg, id_of("amount")), 123));

  const auto all_empty = payload(false, false);
  Decoded no_legs(all_empty.substr(1));
  check("legs/all_empty", no_legs.ok() && at_id(no_legs.root(), id_of("legs")) == nullptr);

  const auto full = payload(true, true);
  Decoded both(full.substr(1));
  const auto *full_legs = at_id(both.root(), id_of("legs"));
  const auto *full_first = elem(full_legs, 0);
  const auto *full_second = elem(full_legs, 1);
  check("legs/both_filled", both.ok() && full_legs != nullptr &&
                                full_legs->type == msgpack::type::ARRAY &&
                                full_legs->via.array.size == 2 &&
                                full_first != nullptr && full_second != nullptr &&
                                is_uint(at_id(*full_first, id_of("amount")), 123) &&
                                is_uint(at_id(*full_second, id_of("amount")), 456));
}

void test_tx_hash_order() {
  Transaction tx_a, tx_b, tx_z;
  tx_a.hash = "A"; tx_a.lt = 2;
  tx_b.hash = "B"; tx_b.lt = 3;
  tx_z.hash = "Z"; tx_z.lt = 1;
  EventNode node_a, node_b, node_z, initiating;
  node_a.tx = &tx_a; node_a.is_tick_tock = true;
  node_b.tx = &tx_b; node_b.is_tick_tock = true;
  node_z.tx = &tx_z; node_z.is_tick_tock = true;
  initiating.tx = &tx_z;
  Block block;
  block.btype = "tick_tock";
  block.event_nodes = {&node_z, &node_b, &node_a};
  block.initiating_event_node = &initiating;
  Action action;
  check("hash_order/initiating_build", build_action(&block, action));
  check("hash_order/initiating_first_unique", action.tx_hashes == std::vector<std::string>({"Z", "A", "B"}));
  Decoded encoded(serialize_redis_actions({action}, make_view({EmuFinality::finalized})));
  const auto *row = first_row(encoded);
  check("hash_order/initiating_wire", encoded.ok() && row != nullptr &&
                                       is_hash_array(at(*row, "tx_hashes"), {"Z", "A", "B"}));

  block.initiating_event_node = nullptr;
  Action rooted;
  check("hash_order/rooted_build", build_action(&block, rooted));
  check("hash_order/rooted_first", rooted.tx_hashes == std::vector<std::string>({"Z", "A", "B"}));

  tx_b.lt = 1;
  Action tied;
  check("hash_order/tied_build", build_action(&block, tied));
  check("hash_order/tied_smaller_hash_first", tied.tx_hashes == std::vector<std::string>({"B", "A", "Z"}));
}

void test_compact_types_and_errors() {
  auto encoded = [](Value::Fields fields) {
    Action action = base_action();
    action.data = Value::make_dict(std::move(fields));
    return encode_action_payload(action);
  };
  auto fails = [&](const char *label, Value::Fields fields, const char *key) {
    try {
      encoded(std::move(fields));
      check(label, false);
    } catch (const std::runtime_error &error) {
      check(label, std::string(error.what()).find(key) != std::string::npos);
    }
  };
  check("compact/empty_known_bytes", encoded({}) == "\x01\x80");
  check("compact/string_known_bytes", encoded({{"comment", Value::make_str("x")}}) == "\x01\x81\x0d\xa1x");
  check("compact/bool_known_bytes", encoded({{"comment_encrypted", Value::make_bool(true)}}) == "\x01\x81\x0e\xc3");
  check("compact/uint_known_bytes", encoded({{"query_id", Value::make_str("127")}}) == "\x01\x81\x59\x7f");
  check("compact/uint64_max", encoded({{"query_id", Value::make_str("18446744073709551615")}}) ==
        "\x01\x81\x59\xcf\xff\xff\xff\xff\xff\xff\xff\xff");
  check("compact/uint_big_known_bytes", encoded({{"query_id", Value::make_str("18446744073709551616")}}) ==
        std::string("\x01\x81\x59\xc4\x09\x01\x00\x00\x00\x00\x00\x00\x00\x00", 14));
  const std::string huge = "57896044618658097711785492504343953926634992332820282019728792003956564819968";
  const std::string big_payload = encoded({{"order_seqno", Value::make_str(huge)}});
  check("compact/uint256", big_payload.size() == 37 && big_payload.substr(0, 5) == "\x01\x81\x46\xc4\x20" &&
        static_cast<unsigned char>(big_payload[5]) == 0x80 &&
        std::all_of(big_payload.begin() + 6, big_payload.end(), [](char c) { return c == 0; }));
  check("compact/nft_index_signed", encoded({{"nft_index", Value::make_str("-1")}}) ==
        "\x01\x81\x43\xa2-1");
  check("compact/tick_spacing_signed", encoded({{"tick_spacing", Value::make_str("-1")}}) ==
        "\x01\x81\x6f\xa2-1");
  check("compact/boc_base64", encoded({{"custom_payload", Value::make_str("AQID")}}) ==
        "\x01\x81\x0f\xc4\x03\x01\x02\x03");
  check("compact/boc_binary", encoded({{"custom_payload", Value::make_bytes("\x01\x02\x03")}}) ==
        "\x01\x81\x0f\xc4\x03\x01\x02\x03");
  const std::string hash(32, '\xaa');
  const std::string address = "0:" + std::string(64, 'A');
  const std::string addr_payload = encoded({{"asset", Value::make_str(address)}});
  check("compact/address_33", addr_payload.size() == 38 && addr_payload.substr(0, 6) == std::string("\x01\x81\x06\xc4\x21\x00", 6) &&
        addr_payload.substr(6) == hash);
  check("compact/hash_base64", encoded({{"signers_hash", Value::make_str(td::base64_encode(td::Slice(hash)))}}) ==
        std::string("\x01\x81\x65\xc4\x20", 5) + hash);
  check("compact/hash_hex", encoded({{"btc_txid", Value::make_str(std::string(64, 'a'))}}) ==
        std::string("\x01\x81\x0a\xc4\x20", 5) + hash);
  check("compact/hash_0x", encoded({{"asset_id", Value::make_str("0x" + std::string(64, 'a'))}}) ==
        std::string("\x01\x81\x07\xc4\x20", 5) + hash);
  check("compact/ascending_ids", encoded({{"query_id", Value::make_str("1")}, {"comment", Value::make_str("x")}}) ==
        "\x01\x82\x0d\xa1x\x59\x01");
  check("compact/nested_ids", encoded({{"claimed_rewards", Value::make_list({Value::make_dict({
        {"amount", Value::make_str("2")}, {"asset", Value::make_str(address)}})})}}).substr(0, 8) ==
        "\x01\x81\x0c\x91\x82\x02\x02\x06");
  fails("compact/unknown_error", {{"not_registered", Value::make_str("x")}}, "not_registered");
  fails("compact/address_error", {{"asset", Value::make_str("bad")}}, "asset expects W:HEX");
  fails("compact/uint_error", {{"query_id", Value::make_str("01")}}, "query_id expects canonical");
  fails("compact/boc_error", {{"custom_payload", Value::make_str("not-base64")}}, "custom_payload expects padded");
  fails("compact/hash_error", {{"signers_hash", Value::make_str("short")}}, "signers_hash expects 32-byte");
  fails("compact/hash_hex_error", {{"btc_txid", Value::make_str("0x" + std::string(64, 'a'))}}, "btc_txid expects lowercase");
  fails("compact/hash_0x_error", {{"asset_id", Value::make_str(std::string(64, 'a'))}}, "asset_id expects 0x");
  for (const char *key : {"asset", "custom_payload", "signers_hash", "btc_txid", "asset_id", "pubkey", "new_secret_hash"}) {
    fails((std::string("compact/type_error/") + key).c_str(),
          {{key, Value::make_int64(123)}}, "expects");
  }
  fails("compact/amount_type_error", {{"custom_payload", Value::make_amount(td::make_refint(123))}}, "custom_payload expects");
  check("compact/uint_int_allowed", encoded({{"query_id", Value::make_int64(123)}}) == "\x01\x81\x59\x7b");
}

void test_fixed_width_producers() {
  const auto number = td::hex_string_to_int256(std::string(63, 'f'));
  check("producer/precondition_63_digits", td::hex_string(number, false).size() == 63);
  auto pubkey = rt_builtin_uint_hex(Value::make_int(number));
  check("producer/uint_hex_is_general", !pubkey.faulted && pubkey.value.str.size() == 63);
  struct Case { const char *btype; const char *input; const char *key; Value value; const char *prefix; };
  for (const auto &item : std::vector<Case>{
           {"evaa_supply", "asset_id", "asset_id", Value::make_int(number), "0x"},
           {"cocoon_client_change_secret_hash", "new_secret_hash", "new_secret_hash", Value::make_int(number), ""},
           {"tgbtc_new_key", "pubkey", "pubkey", pubkey.value, ""},
           {"tgbtc_dkg_log", "internal_pubkey", "pubkey", pubkey.value, ""},
       }) {
    Block block;
    block.btype = item.btype;
    block.data = Value::make_dict({{item.input, item.value}});
    Action action;
    const std::string label = std::string("producer/") + item.btype;
    check(label + "/build", build_action(&block, action));
    const Value *value = action.data.field(item.key);
    check(label + "/text", value != nullptr && value->t == VType::Str &&
          value->str == std::string(item.prefix) + "0" + std::string(63, 'f'));
    try {
      const std::string bytes = encode_action_payload(action);
      Decoded decoded(bytes.substr(1));
      bool found = false;
      if (decoded.ok() && decoded.root().type == msgpack::type::MAP) {
        for (std::uint32_t i = 0; i < decoded.root().via.map.size; ++i) {
          const auto &entry = decoded.root().via.map.ptr[i].val;
          if (entry.type == msgpack::type::BIN && entry.via.bin.size == 32 &&
              td::hex_encode(td::Slice(entry.via.bin.ptr, entry.via.bin.size)) ==
                  "0" + std::string(63, 'f')) found = true;
        }
      }
      check(label + "/32_bytes_roundtrip", found);
    } catch (const std::exception &) {
      check(label + "/32_bytes_roundtrip", false);
    }
  }
}

void test_cached_payload() {
  Action action = base_action();
  action.data = Value::make_dict({{"query_id", Value::make_str("1")}});
  action.encoded_payload = "sentinel payload bytes";
  Decoded decoded(serialize_redis_actions({action}, make_view({EmuFinality::finalized})));
  const auto *row = first_row(decoded);
  check("payload_cache/redis_sentinel", decoded.ok() && row != nullptr &&
        is_binary(at(*row, "payload"), action.encoded_payload));
}

}  // namespace

int run_action_msgpack_test() {
  g_fail = 0;
  test_empty_actions();
  test_fixed_envelope();
  test_columns_and_payload();
  test_finality_and_child_fields();
  test_positional_legs();
  test_tx_hash_order();
  test_compact_types_and_errors();
  test_fixed_width_producers();
  test_cached_payload();
  std::printf("ACTIONS-MSGPACK-TEST %s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
  return g_fail == 0 ? 0 : 1;
}

}  // namespace mch
