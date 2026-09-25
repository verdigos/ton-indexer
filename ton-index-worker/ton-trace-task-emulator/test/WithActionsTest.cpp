#include "EnginePrep.h"
#include "TonConnectProcessor.h"
#include "emu/EmuClassifierActor.h"

#include "crypto/vm/cp0.h"
#include "td/actor/actor.h"
#include "td/utils/base64.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    std::cerr << "FAIL: " << what << std::endl;
    ++failures;
  }
}

std::string pack_envelope(bool trace, bool with_actions_key) {
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(buf);
  pk.pack_map(2);
  pk.pack(std::string("type"));
  pk.pack(std::string(trace ? "trace" : "tonconnect"));
  pk.pack(std::string("task"));
  pk.pack_map(with_actions_key ? 6 : 5);
  pk.pack(std::string("id"));
  pk.pack(std::string("task-1"));
  if (trace) {
    pk.pack(std::string("boc"));
    pk.pack(std::string("te6ccgEBAQEAAgAAAA=="));
    pk.pack(std::string("ignore_chksig"));
    pk.pack(false);
  } else {
    pk.pack(std::string("from"));
    pk.pack(std::string("0:0000000000000000000000000000000000000000000000000000000000000000"));
    pk.pack(std::string("messages"));
    pk.pack_array(0);
  }
  pk.pack(std::string("detect_interfaces"));
  pk.pack(true);
  pk.pack(std::string("include_code_data"));
  pk.pack(false);
  if (with_actions_key) {
    pk.pack(std::string("with_actions"));
    pk.pack(true);
  }
  return std::string(buf.data(), buf.size());
}

template <class T>
T decode_task(const std::string& bytes) {
  auto oh = msgpack::unpack(bytes.data(), bytes.size());
  TaskEnvelope env;
  oh.get().convert(env);
  T task;
  env.task.convert(task);
  return task;
}

struct FixtureNode {
  std::string address, boc;
  std::uint32_t mc_seqno{};
  MSGPACK_DEFINE_MAP(address, boc, mc_seqno);
};

struct FixtureInterface {
  std::string kind, address, index, collection_address, owner_address, code_hash;
  std::string mp_addr, nft_addr, nft_owner, last_bid;
  std::map<std::string, std::string> content;
  bool init{};
  MSGPACK_DEFINE_MAP(kind, address, index, collection_address, owner_address, code_hash,
                     mp_addr, nft_addr, nft_owner, last_bid, content, init);
};

struct FixtureAction {
  std::string id, payload_hex, wire_hex;
  MSGPACK_DEFINE_MAP(id, payload_hex, wire_hex);
};

struct ActorFixture {
  std::string trace_id;
  std::vector<FixtureNode> nodes;
  std::vector<FixtureInterface> interfaces;
  std::vector<FixtureAction> actions;
  MSGPACK_DEFINE_MAP(trace_id, nodes, interfaces, actions);
};

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string unhex(const std::string& hex) {
  std::string out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
  }
  return out;
}

block::StdAddress address(const std::string& text) {
  auto result = block::StdAddress::parse(td::Slice(text));
  check(result.is_ok(), "fixture address parses: " + text);
  return result.is_ok() ? result.move_as_ok() : block::StdAddress{};
}

td::Bits256 bits(const std::string& hex) {
  td::Bits256 result;
  result.set_zero();
  check(hex.empty() || result.from_hex(hex) == 256, "fixture code hash parses");
  return result;
}

std::optional<block::StdAddress> optional_address(const std::string& text) {
  if (text.empty()) return std::nullopt;
  return address(text);
}

std::shared_ptr<const mch::TieredLookupSource::InterfaceMap> make_interfaces(
    const std::vector<FixtureInterface>& rows) {
  mch::TieredLookupSource::InterfaceMap result;
  for (const auto& row : rows) {
    const auto account = address(row.address);
    if (row.kind == "nft_item") {
      schema::NFTItemDataV2 nft{};
      nft.address = account;
      nft.init = row.init;
      nft.index = td::string_to_int256(row.index);
      nft.collection_address = optional_address(row.collection_address);
      nft.owner_address = optional_address(row.owner_address);
      nft.content = row.content;
      nft.code_hash = bits(row.code_hash);
      result[account].emplace_back(std::move(nft));
    } else if (row.kind == "nft_auction") {
      schema::GetGemsNftAuctionData auction{};
      auction.address = account;
      auction.mp_addr = address(row.mp_addr);
      auction.nft_addr = address(row.nft_addr);
      auction.nft_owner = optional_address(row.nft_owner);
      auction.last_bid = td::string_to_int256(row.last_bid);
      auction.code_hash = bits(row.code_hash);
      result[account].emplace_back(std::move(auction));
    } else {
      check(false, "unsupported fixture interface: " + row.kind);
    }
  }
  return std::make_shared<const mch::TieredLookupSource::InterfaceMap>(std::move(result));
}

ActorFixture load_fixture() {
  // Refresh with: indexer/venv/bin/python devtools/bk-tools/update_emu_actor_fixture.py
  const auto path = std::filesystem::path(__FILE__).parent_path() / "data" / "emu_actor.msgpack";
  const auto bytes = read_file(path);
  check(!bytes.empty(), "actor fixture loads");
  ActorFixture fixture;
  if (!bytes.empty()) msgpack::unpack(bytes.data(), bytes.size()).get().convert(fixture);
  return fixture;
}

const msgpack::object* field(const msgpack::object& map, const std::string& name) {
  if (map.type != msgpack::type::MAP) return nullptr;
  for (std::uint32_t i = 0; i < map.via.map.size; ++i) {
    const auto& item = map.via.map.ptr[i];
    if (item.key.type == msgpack::type::STR && item.key.as<std::string>() == name) return &item.val;
  }
  return nullptr;
}

void test_real_actor() {
  auto fixture = load_fixture();
  if (fixture.nodes.empty()) return;

  mch::EmuTraceView view;
  view.trace_id = fixture.trace_id;
  view.interfaces = make_interfaces(fixture.interfaces);
  for (const auto& node : fixture.nodes) {
    auto boc = td::base64_decode(node.boc);
    check(boc.is_ok(), "transaction BOC base64 decodes");
    if (boc.is_error()) return;
    view.nodes.push_back({address(node.address),
                          std::make_shared<const std::string>(boc.move_as_ok()),
                          node.mc_seqno, mch::EmuFinality::finalized});
  }

  auto prep = mch::make_engine_prep();
  check(prep.is_ok(), "engine prep succeeds");
  if (prep.is_error()) return;

  mch::EmuClassifyResult result;
  bool answered = false;
  td::actor::Scheduler scheduler({1});
  td::actor::ActorOwn<mch::EmuClassifierActor> actor;
  scheduler.run_in_context([&] {
    actor = td::actor::create_actor<mch::EmuClassifierActor>(
        "EmuClassifierActorTest", mch::EmuClassifierConfig{prep.move_as_ok(), 1, false});
    td::actor::send_closure(
        actor, &mch::EmuClassifierActor::classify, std::move(view), mch::emu_now_us(),
        td::PromiseCreator::lambda([&](td::Result<mch::EmuClassifyResult> value) {
          check(value.is_ok(), "actor returns a result");
          if (value.is_ok()) result = value.move_as_ok();
          answered = true;
          td::actor::SchedulerContext::get().stop();
        }));
  });
  scheduler.run();
  scheduler.run_in_context([&] { actor.reset(); });

  check(answered, "actor answers exactly once");
  check(result.outcome == mch::EmuClassifyOutcome::classified, "actor outcome is classified");
  check(result.skipped_payload_actions == 0, "actor skips no payload actions");
  check(!result.payload.actions_blob.empty(), "actor produced Redis wire bytes");
  if (result.payload.actions_blob.empty()) return;
  auto decoded = msgpack::unpack(result.payload.actions_blob.data(), result.payload.actions_blob.size());
  const auto& rows = decoded.get();
  check(rows.type == msgpack::type::ARRAY, "actor wire is an action array");
  if (rows.type == msgpack::type::ARRAY) {
    check(rows.via.array.size == fixture.actions.size(), "actor action count matches batch golden");
  }
  for (const auto& expected : fixture.actions) {
    bool found = false;
    if (rows.type == msgpack::type::ARRAY) {
      for (std::uint32_t i = 0; i < rows.via.array.size; ++i) {
        const auto& row = rows.via.array.ptr[i];
        const auto* id = field(row, "action_id");
        const auto* payload = field(row, "payload");
        if (id && id->as<std::string>() == expected.id) {
          found = true;
          const auto expected_payload = unhex(expected.payload_hex);
          const bool payload_matches = payload && payload->type == msgpack::type::BIN &&
                                       payload->via.bin.size == expected_payload.size() &&
                                       std::string(payload->via.bin.ptr, payload->via.bin.size) == expected_payload;
          check(payload_matches, "compact payload bytes match for " + expected.id);

          const auto expected_wire = unhex(expected.wire_hex);
          msgpack::sbuffer wire;
          msgpack::packer<msgpack::sbuffer> packer(wire);
          packer.pack_array(1);
          packer.pack(row);
          check(std::string(wire.data(), wire.size()) == expected_wire,
                "Redis wire row matches for " + expected.id);
          break;
        }
      }
    }
    check(found, "expected action is present: " + expected.id);
  }
}

}  // namespace

int main() {
  CHECK(vm::init_op_cp0());

  check(decode_task<TraceTask>(pack_envelope(true, true)).with_actions, "trace task with with_actions key");
  check(!decode_task<TraceTask>(pack_envelope(true, false)).with_actions, "trace task without with_actions key");
  check(decode_task<TonConnectTraceTask>(pack_envelope(false, true)).with_actions, "tonconnect task with with_actions key");
  check(!decode_task<TonConnectTraceTask>(pack_envelope(false, false)).with_actions, "tonconnect task without with_actions key");

  test_real_actor();
  if (failures != 0) return 1;
  std::cout << "task_emulator_with_actions: OK" << std::endl;
  return 0;
}
