#include "td/utils/port/signals.h"
#include "td/actor/actor.h"
#include "td/utils/OptionParser.h"
#include "td/utils/logging.h"
#include "td/utils/check.h"
#include "td/utils/port/path.h"

#include "crypto/vm/cp0.h"

#include "BatchRunner.h"
#include "ClassifyCore.h"
#include "DataParser.h"
#include "DbScanner.h"
#include "EnginePrep.h"
#include "TraceAssembler.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler().ensure();
  CHECK(vm::init_op_cp0());
  auto r_prep = mch::make_engine_prep();
  if (r_prep.is_error()) {
    LOG(FATAL) << "MCH engine prep failed: " << r_prep.move_as_error();
  }
  std::shared_ptr<const mch::MchEnginePrep> prep = r_prep.move_as_ok();

  std::string db_root;
  std::string workdir;
  std::string out_path;
  std::uint32_t seqno_from = 0;
  std::uint32_t seqno_to = 0;
  std::uint32_t prewarm = 50;
  std::int32_t threads = 7;
  bool no_tier2 = false;
  bool emit_tick_tock = false;
  bool tier1_completion_only = false;

  auto uint_option = [](std::uint32_t &dst, const char *name) {
    return [&dst, name](td::Slice v) {
      try {
        dst = static_cast<std::uint32_t>(std::stoul(v.str()));
      } catch (...) {
        return td::Status::Error(PSLICE() << "bad value for --" << name << ": not a number");
      }
      return td::Status::OK();
    };
  };

  td::OptionParser p;
  p.set_description("mch trace assembly over a node RocksDB seqno range");
  p.add_option('D', "db", "node database root", [&](td::Slice v) { db_root = v.str(); });
  p.add_option('W', "workdir", "private secondary-mode workdir", [&](td::Slice v) { workdir = v.str(); });
  p.add_option('o', "out", "reserved output path", [&](td::Slice v) { out_path = v.str(); });
  p.add_checked_option('f', "from", "first masterchain seqno (trace end attribution)",
                       uint_option(seqno_from, "from"));
  p.add_checked_option('t', "to", "last masterchain seqno (inclusive)", uint_option(seqno_to, "to"));
  p.add_checked_option('\0', "prewarm", "extra seqnos read before --from to seed pending trace edges (default 50)",
                       uint_option(prewarm, "prewarm"));
  p.add_checked_option('\0', "threads", "scheduler threads (default 7)", [&](td::Slice v) {
    try {
      threads = std::stoi(v.str());
    } catch (...) {
      return td::Status::Error("bad value for --threads: not a number");
    }
    return td::Status::OK();
  });
  p.add_option('\0', "no-tier2", "disable shard-state tier-2 interface lookups (diagnostic)",
               [&]() { no_tier2 = true; });
  p.add_option('\0', "emit-tick-tock", "classify and emit tick-tock-rooted traces (production stores none; policy undecided)",
               [&]() { emit_tick_tock = true; });
  p.add_option('\0', "tier1-completion-only", "tier-1 lookups see only the completion block's interfaces (A/B diagnostic)",
               [&]() { tier1_completion_only = true; });

  auto status = p.run(argc, argv);
  if (status.is_error()) {
    LOG(ERROR) << status.error().message();
    return 1;
  }
  if (db_root.empty() || workdir.empty() || out_path.empty() || seqno_from == 0 || seqno_to < seqno_from ||
      threads <= 0) {
    LOG(ERROR) << "required: --db, --workdir, --out, --from, --to (with to >= from), and --threads > 0";
    return 1;
  }

  const auto read_start = std::max<std::uint32_t>(1, seqno_from > prewarm ? seqno_from - prewarm : 1);

  LOG(INFO) << "ton-mch-index: db=" << db_root << " workdir=" << workdir << " range=[" << seqno_from
            << "," << seqno_to << "] prewarm=" << prewarm << " threads=" << threads
            << " out=" << out_path << " tier2=" << (no_tier2 ? "off" : "on");

  auto mkdir_status = td::mkpath(workdir + "/");
  if (mkdir_status.is_error()) {
    LOG(ERROR) << "Failed to create workdir " << workdir << ": " << mkdir_status.move_as_error();
    return 7;
  }

  auto exit_code = std::make_shared<std::atomic_int>(7);
  auto watcher = td::create_shared_destructor([] { td::actor::SchedulerContext::get().stop(); });

  td::actor::Scheduler scheduler({threads});
  td::actor::ActorOwn<ParseManager> parse_manager;
  td::actor::ActorOwn<DbScanner> db_scanner;
  td::actor::ActorOwn<TraceAssembler> trace_assembler;

  scheduler.run_in_context(
      [&] { parse_manager = td::actor::create_actor<ParseManager>("mch_parse_manager"); });
  scheduler.run_in_context([&] {
    db_scanner = td::actor::create_actor<DbScanner>("mch_db_scanner", db_root, dbs_secondary,
                                                    workdir + "/secondary_logs", 60.0f, false);
  });
  scheduler.run_in_context([&] {
    trace_assembler =
        td::actor::create_actor<TraceAssembler>("mch_trace_assembler", workdir + "/trace_assembler");
  });
  scheduler.run_in_context([&, watcher = std::move(watcher)] {
    td::actor::create_actor<BatchRunner>("mch_batch_runner", db_scanner.get(), parse_manager.get(),
                                         trace_assembler.get(), prep, out_path, read_start, seqno_from,
                                         seqno_to, !no_tier2, emit_tick_tock, tier1_completion_only,
                                         exit_code, std::move(watcher))
        .release();
  });

  scheduler.run();
  return exit_code->load();
}
