// --actions-msgpack-test: action-row envelope and payload parity checks.
#include "RedisActionSerializeTest.h"

#include "ActionBuild.h"
#include "ActionPayload.h"
#include "ClassifyCore.h"
#include "EnginePrep.h"
#include "GenMatchers.h"
#include "RedisActionSerialize.h"
#include "Value.h"
#include "emu/EmuTypes.h"
#include "fixtures/FixtureLoader.h"
#include "fixtures/FixtureLookupSource.h"

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
  check("fixed/parent_action_id_nil", at(*row, "parent_action_id") != nullptr &&
                                           at(*row, "parent_action_id")->type == msgpack::type::NIL);
  check("fixed/payload_binary", at(*row, "payload") != nullptr &&
                                   at(*row, "payload")->type == msgpack::type::BIN);
  check("fixed/omits_data", absent(*row, "data"));
  check("fixed/omits_legacy_composite", absent(*row, "ton_transfer_data"));
  check("fixed/exact_key_count", row->via.map.size == 13);
}

void test_columns_and_payload() {
  Action action = base_action();
  action.type = "jetton_transfer";
  action.source = Value::make_account_raw(kAddrA);
  action.destination = Value::make_account_raw(kAddrB);
  action.asset = Value::make_asset_jetton(kAddrA);
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

  const std::string expected = encode_action_payload(action);
  check("columns/payload_matches_batch", is_binary(at(*row, "payload"), expected));
  const std::string wire_payload = binary_string(at(*row, "payload"));
  check("columns/payload_codec", !wire_payload.empty() && wire_payload.front() == '\x01');
  Decoded payload(wire_payload.size() > 1 ? wire_payload.substr(1) : std::string{});
  check("columns/payload_decodes", payload.ok() && payload.root().type == msgpack::type::MAP);
  const auto *data = at(payload.root(), "data");
  check("columns/data_map", data != nullptr && data->type == msgpack::type::MAP);
  if (data == nullptr) return;
  check("columns/query_id_string", is_str(at(*data, "query_id"), "18446744073709551615"));
  check("columns/comment", is_str(at(*data, "comment"), "hello"));
  check("columns/no_legacy_wrapper", absent(payload.root(), "ton_transfer_data"));
}

void test_finality_and_child_fields() {
  Action action = base_action();
  action.parent_action_id = "QIoX0flkEEHBMhk2Z6QlnxrXlWXACZBCee6T8YKYSKE=";
  action.ancestor_type = {"jvault_stake", "teleitem_start_auction"};
  Decoded decoded(serialize_redis_actions(
      {action}, make_view({EmuFinality::finalized, EmuFinality::emulated, EmuFinality::confirmed})));
  const auto *row = first_row(decoded);
  check("child/decodes", decoded.ok() && row != nullptr);
  if (row == nullptr) return;
  check("child/parent_action_id", is_str(at(*row, "parent_action_id"), action.parent_action_id));
  check("child/ancestor_type", elem(at(*row, "ancestor_type"), 0) != nullptr &&
                                  is_str(elem(at(*row, "ancestor_type"), 0), "jvault_stake"));
  check("child/finality_min", is_uint(at(*row, "finality"), 0));
  check("child/payload_matches", is_binary(at(*row, "payload"), encode_action_payload(action)));
}

void test_fixture_emulator_batch_parity() {
#ifdef MCH_ACTION_TRACES
  // Fixture loading is the batch side. The emulator actor's write-back path
  // calls serialize_redis_actions on these same Action rows; the independent
  // batch bytes come from encode_action_payload below.
  const std::string fixture = std::string(MCH_ACTION_TRACES) +
                              "/-02fS1r1ED0E0KV9ZwwRfYp3HBDPOiZIFgUaXav1I-I=.lz4";
  auto loaded = load_trace_context(fixture);
  check("fixture/load", loaded.is_ok());
  if (loaded.is_error()) return;
  auto context = loaded.move_as_ok();

  const auto &matchers = gen_matchers_ir();
  const auto setup = prepare_classify(matchers);
  FixtureLookupSource source(&context.trace.interfaces, &context.trace.code_hashes);
  auto classified = classify_trace(context, matchers, setup, source);
  check("fixture/classifies", !classified.failure);
  if (classified.failure) return;

  std::vector<Action> actions;
  for (const auto &row : classified.action_rows) {
    Action action;
    if (build_action(row, action)) actions.push_back(std::move(action));
  }
  if (classified.unknown_trace) actions.push_back(create_unknown_action(context.trace));
  check("fixture/has_rows", !actions.empty());

  EmuTraceView view;
  view.trace_id = context.trace.trace_id;
  view.nodes.resize(1);
  view.nodes.front().finality = EmuFinality::finalized;
  Decoded encoded(serialize_redis_actions(actions, view));
  check("fixture/serialized", encoded.ok() && encoded.root().via.array.size == actions.size());
  if (!encoded.ok()) return;

  for (std::size_t i = 0; i < actions.size(); i++) {
    const auto *row = elem(&encoded.root(), static_cast<std::uint32_t>(i));
    check("fixture/row_payload_" + std::to_string(i),
          row != nullptr && is_binary(at(*row, "payload"), encode_action_payload(actions[i])) &&
              absent(*row, "data") && absent(*row, "ton_transfer_data"));
  }
#else
  check("fixture/path_macro", false);
#endif
}

void test_wire_fixture() {
  Decoded decoded(wire_fixture_bytes());
  const auto *row = first_row(decoded);
  check("wire/decodes", decoded.ok() && row != nullptr);
  check("wire/payload_binary", row != nullptr && at(*row, "payload") != nullptr &&
                                   at(*row, "payload")->type == msgpack::type::BIN);
}

void test_determinism() {
  Action action = base_action();
  action.data = Value::make_dict({{"protocol", Value::make_str("dedust")},
                                  {"tokens_burnt", Value::make_amount(td::make_refint(9))}});
  const auto view = make_view({EmuFinality::confirmed, EmuFinality::finalized});
  const auto first = serialize_redis_actions({action}, view);
  const auto second = serialize_redis_actions({action}, view);
  check("determinism/byte_identical", !first.empty() && first == second);
}

}  // namespace

std::string wire_fixture_bytes() {
  Action action = base_action();
  action.type = "jetton_transfer";
  action.parent_action_id = "QIoX0flkEEHBMhk2Z6QlnxrXlWXACZBCee6T8YKYSKE=";
  action.ancestor_type = {"jvault_stake"};
  action.source = Value::make_account_raw(kAddrA);
  action.destination = Value::make_account_raw(kAddrB);
  action.asset = Value::make_asset_jetton(kAddrA);
  action.amount = Value::make_amount(td::string_to_int256(
      std::string("340282366920938463463374607431768211456")));
  action.opcode = Value::make_int64(0x0f8a7ea5);
  action.data = Value::make_dict({
      {"query_id", Value::make_int(td::string_to_int256(std::string("18446744073709551615")))},
      {"response_destination", Value::make_account_raw(kAddrB)},
      {"forward_amount", Value::make_amount(td::make_refint(400000000))},
      {"comment", Value::make_str("hello")},
      {"comment_encrypted", Value::make_bool(false)},
  });
  return serialize_redis_actions({action},
                                 make_view({EmuFinality::confirmed, EmuFinality::finalized}));
}

int run_action_msgpack_test() {
  g_fail = 0;
  test_empty_actions();
  test_fixed_envelope();
  test_columns_and_payload();
  test_finality_and_child_fields();
  test_fixture_emulator_batch_parity();
  test_wire_fixture();
  test_determinism();
  std::printf("ACTIONS-MSGPACK-TEST %s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
  return g_fail == 0 ? 0 : 1;
}

}  // namespace mch
