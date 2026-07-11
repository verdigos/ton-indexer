// ton-mch-engine — C++ runtime for the MCH matcher IR (docs/mch-ir-spec.md).
//
// Step 1: minimal executable in the TON actor environment. Inherits the
// dependency set of ton-smc-scanner via tondb-scanner (ton libs, msgpack).
// Step 2: load .lz4 trace fixtures (LZ4-frame msgpack) and print transactions.

#include "td/utils/OptionParser.h"
#include "td/utils/logging.h"
#include "td/utils/port/signals.h"
#include "td/utils/check.h"
#include "td/actor/actor.h"
#include "crypto/vm/cp0.h"

#include "BlockTree.h"
#include "BuildSmoke.h"
#include "Classify.h"
#include "BuildVectorRunner.h"
#include "MatchRunner.h"
#include "ParseDump.h"
#include "SchemaGate.h"
#include "ShapeTest.h"
#include "TraceLoader.h"
#include "VectorRunner.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

void print_trace(const mch::Trace &trace) {
  LOG(INFO) << "trace_id: " << trace.trace_id << ", transactions: " << trace.transactions.size();
  std::vector<const mch::Transaction *> txs;
  for (const auto &tx : trace.transactions) {
    txs.push_back(tx.get());
  }
  std::sort(txs.begin(), txs.end(),
            [](const mch::Transaction *a, const mch::Transaction *b) { return a->lt < b->lt; });
  for (const auto *tx : txs) {
    const mch::Message *in_msg = nullptr;
    size_t out_count = 0;
    for (const auto &m : tx->messages) {
      if (m->direction == "in") {
        in_msg = m.get();
      } else {
        out_count++;
      }
    }
    td::StringBuilder sb;
    sb << "tx lt=" << tx->lt << " account=" << tx->account << " descr=" << tx->descr;
    if (tx->aborted) {
      sb << " ABORTED";
    }
    if (in_msg != nullptr) {
      auto op = in_msg->opcode32();
      sb << " | in: ";
      if (op) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%08x", *op);
        sb << buf;
      } else {
        sb << "(no opcode)";
      }
      sb << " from " << (in_msg->source ? *in_msg->source : "(ext)");
    }
    sb << " | out msgs: " << out_count;
    LOG(INFO) << sb.as_cslice();
  }
}

void print_block(const mch::Block *block, int depth) {
  td::StringBuilder sb;
  for (int i = 0; i < depth; i++) {
    sb << "  ";
  }
  sb << block->btype;
  if (block->opcode) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), " 0x%08x", *block->opcode);
    sb << buf;
  }
  if (block->failed) {
    sb << " FAILED";
  }
  sb << " (nodes: " << block->event_nodes.size() << ", lt: " << block->min_lt << ")";
  LOG(INFO) << sb.as_cslice();
  for (const auto *n : block->next_blocks) {
    print_block(n, depth + 1);
  }
}

}  // namespace

class MchEngineWorker : public td::actor::Actor {
 public:
  MchEngineWorker(std::string ir_path, std::vector<std::string> trace_paths)
      : ir_path_(std::move(ir_path)), trace_paths_(std::move(trace_paths)) {
  }

  void start_up() override {
    LOG(INFO) << "ton-mch-engine: hello from the actor environment";
    if (!ir_path_.empty()) {
      LOG(INFO) << "IR artifact path (not loaded yet): " << ir_path_;
    }
    for (const auto &path : trace_paths_) {
      auto r_ctx = mch::load_trace_context(path);
      if (r_ctx.is_error()) {
        LOG(ERROR) << "failed to load trace " << path << ": " << r_ctx.move_as_error();
        continue;
      }
      auto ctx = r_ctx.move_as_ok();
      LOG(INFO) << "loaded " << path;
      print_trace(ctx.trace);

      if (ctx.tree.unlinked != 0) {
        LOG(WARNING) << "event tree has " << ctx.tree.unlinked
                     << " nodes unreachable from root (orphan subtrees)";
      }
      LOG(INFO) << "block tree (" << ctx.arena.blocks.size() << " leaf blocks):";
      print_block(ctx.root, 0);
    }
    td::actor::SchedulerContext::get()->stop();
    stop();
  }

 private:
  std::string ir_path_;
  std::vector<std::string> trace_paths_;
};

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler().ensure();

  CHECK(vm::init_op_cp0());

  td::uint32 threads = 2;
  std::string ir_path;
  std::string vectors_path;
  std::string vectors_gen_path;
  std::string build_vectors_path;
  bool match_mode = false;
  bool match_gen = false;
  bool build_smoke_mode = false;
  bool classify_mode = false;
  bool classify_schema_mode = false;
  bool classify_prod_mode = false;
  bool actions_mode = false;
  bool actions_schema_mode = false;
  bool actions_prod_mode = false;
  bool parse_dump_mode = false;
  bool shape_test_mode = false;
  std::vector<std::string> trace_paths;

  td::OptionParser p;
  p.set_description("MCH matcher IR engine (PoC)");
  p.add_option('V', "vectors", "Run expression conformance vectors through the interpreter and exit",
               [&](td::Slice value) { vectors_path = value.str(); });
  p.add_option('\0', "vectors-gen",
               "Run expression conformance vectors through GENERATED code and exit",
               [&](td::Slice value) { vectors_gen_path = value.str(); });
  p.add_option('\0', "build-vectors",
               "Run build-program conformance vectors through GENERATED build fns and exit",
               [&](td::Slice value) { build_vectors_path = value.str(); });
  p.add_option('\0', "match", "Match-phase dump: -I <artifact> -T <fixture|dir> (repeatable)",
               [&]() { match_mode = true; });
  p.add_option('\0', "match-gen",
               "Match-phase dump using GENERATED where_expr functions (else interpreter)",
               [&]() {
                 match_mode = true;
                 match_gen = true;
               });
  p.add_option('\0', "build-smoke",
               "Build-phase smoke dump (two-phase lookups): -I <artifact> -T <fixture|dir>",
               [&]() { build_smoke_mode = true; });
  p.add_option('\0', "classify",
               "End-to-end classify dump (match+build+merge loop): -I <artifact> -T <fixture|dir>",
               [&]() { classify_mode = true; });
  p.add_option('\0', "classify-schema",
               "Classify via the production SchemaTraceLoader path (A/B gate vs --classify): "
               "-I <artifact> -T <fixture|dir>",
               [&]() { classify_schema_mode = true; });
  p.add_option('\0', "actions",
               "Serialized-Action-row dump (Phase E part 2): -I <artifact> -T <fixture|dir>",
               [&]() { actions_mode = true; });
  p.add_option('\0', "actions-schema",
               "Action-row dump via the production SchemaTraceLoader path (A/B gate vs "
               "--actions): -I <artifact> -T <fixture|dir>",
               [&]() { actions_schema_mode = true; });
  p.add_option('\0', "classify-prod",
               "Classify via the FULL production path (SchemaTraceLoader + "
               "ParsedBlockLookupSource; A/B gate vs --classify): -I <artifact> -T <fixture|dir>",
               [&]() { classify_prod_mode = true; });
  p.add_option('\0', "actions-prod",
               "Action-row dump via the FULL production path (A/B gate vs --actions): "
               "-I <artifact> -T <fixture|dir>",
               [&]() { actions_prod_mode = true; });
  p.add_option('\0', "parse-dump", "Message-parse dump: -T <fixture|dir> (repeatable)",
               [&]() { parse_dump_mode = true; });
  p.add_option('\0', "shape-test",
               "Shaper substrate self-test (Block::insert_between + getgems_proxy_insert)",
               [&]() { shape_test_mode = true; });
  p.add_option('\0', "help", "prints help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_checked_option('t', "threads", "Scheduler threads (default: 2)", [&](td::Slice value) {
    int v;
    try {
      v = std::stoi(value.str());
    } catch (...) {
      return td::Status::Error("bad value for --threads: not a number");
    }
    threads = v;
    return td::Status::OK();
  });
  p.add_option('I', "ir", "Path to the MCH IR artifact (json)", [&](td::Slice value) {
    ir_path = value.str();
  });
  p.add_option('T', "trace", "Path to an .lz4 trace fixture (repeatable)", [&](td::Slice value) {
    trace_paths.push_back(value.str());
  });

  auto S = p.run(argc, argv);
  if (S.is_error()) {
    LOG(ERROR) << "failed to parse options: " << S.move_as_error();
    std::_Exit(2);
  }

  if (!vectors_path.empty()) {
    // Expression-VM conformance mode (interpreter): no actor scheduler needed.
    return mch::run_vectors(vectors_path);
  }

  if (!vectors_gen_path.empty()) {
    // Expression-VM conformance mode (generated code): no actor scheduler needed.
    return mch::run_vectors_gen(vectors_gen_path);
  }

  if (!build_vectors_path.empty()) {
    // Build-program conformance mode (generated build fns): no scheduler needed.
    return mch::run_build_vectors(build_vectors_path);
  }

  if (match_mode) {
    // Match-phase dump mode: no actor scheduler needed.
    return mch::run_match(ir_path, trace_paths, match_gen);
  }

  if (build_smoke_mode) {
    // Build-phase smoke mode (two-phase lookups): no scheduler needed.
    return mch::run_build_smoke(ir_path, trace_paths);
  }

  if (classify_schema_mode) {
    return mch::run_classify_schema(ir_path, trace_paths);
  }

  if (actions_schema_mode) {
    return mch::run_actions_schema(ir_path, trace_paths);
  }

  if (classify_prod_mode) {
    return mch::run_classify_prod(ir_path, trace_paths);
  }

  if (actions_prod_mode) {
    return mch::run_actions_prod(ir_path, trace_paths);
  }

  if (classify_mode) {
    // End-to-end classify mode: no scheduler needed.
    return mch::run_classify(ir_path, trace_paths);
  }

  if (actions_mode) {
    // Serialized-Action-row dump (Phase E part 2): no scheduler needed.
    return mch::run_actions(ir_path, trace_paths);
  }

  if (parse_dump_mode) {
    // Message-parse dump mode: no actor scheduler needed.
    return mch::run_parse_dump(trace_paths);
  }

  if (shape_test_mode) {
    // Shaper substrate self-test: no actor scheduler needed.
    return mch::run_shape_test();
  }

  td::actor::Scheduler scheduler({threads});
  scheduler.run_in_context([&] {
    td::actor::create_actor<MchEngineWorker>("mch-engine", ir_path, std::move(trace_paths)).release();
  });
  while (scheduler.run(1)) {
  }
  return 0;
}
