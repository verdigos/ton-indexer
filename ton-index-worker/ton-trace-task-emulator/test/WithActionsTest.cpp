#include "TonConnectProcessor.h"
#include "EmuTraceViewBuilder.h"
#include "crypto/vm/cells/CellBuilder.h"
#include "crypto/vm/cp0.h"

#include <iostream>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
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
  msgpack::object_handle oh = msgpack::unpack(bytes.data(), bytes.size());
  TaskEnvelope env;
  oh.get().convert(env);
  T task;
  env.task.convert(task);
  return task;
}

std::unique_ptr<TraceNode> make_node(td::uint32 tag) {
  auto node = std::make_unique<TraceNode>();
  node->transaction_root = vm::CellBuilder().store_long(tag, 32).finalize();
  node->mc_block_seqno = 7;
  return node;
}

}  // namespace

int main() {
  CHECK(vm::init_op_cp0());

  check(decode_task<TraceTask>(pack_envelope(true, true)).with_actions, "trace task with with_actions key");
  check(!decode_task<TraceTask>(pack_envelope(true, false)).with_actions, "trace task without with_actions key");
  check(decode_task<TonConnectTraceTask>(pack_envelope(false, true)).with_actions, "tonconnect task with with_actions key");
  check(!decode_task<TonConnectTraceTask>(pack_envelope(false, false)).with_actions, "tonconnect task without with_actions key");

  Trace trace;
  trace.root = make_node(1);
  trace.root->children.push_back(make_node(2));
  trace.root->children.push_back(make_node(3));
  auto view_r = build_emu_trace_view(trace);
  check(view_r.is_ok(), "view builds");
  if (view_r.is_ok()) {
    auto view = view_r.move_as_ok();
    check(view.nodes.size() == 3, "view node count");
    for (const auto& node : view.nodes) {
      check(node.tx_boc && !node.tx_boc->empty(), "node tx_boc non-empty");
      check(node.mc_seqno == 7, "node mc_seqno");
      check(node.finality == mch::EmuFinality::emulated, "node finality");
    }
  }

  if (failures != 0) {
    return 1;
  }
  std::cout << "task_emulator_with_actions: OK" << std::endl;
  return 0;
}
