#include "td/utils/port/signals.h"
#include "td/utils/OptionParser.h"
#include "td/utils/format.h"
#include "td/utils/logging.h"
#include "td/utils/check.h"

#include "crypto/vm/cp0.h"

#include "DbScanner.h"
#include "TraceScheduler.h"
#include "TraceInserter.h"
#include "emu/EmuShadow.h"


int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler().ensure();

  CHECK(vm::init_op_cp0());

  // options
  std::string db_root;
  std::string working_dir;
  td::uint32 threads = 7;
  std::string redis_dsn = "tcp://127.0.0.1:6379";
  std::string redis_channel = "";
  
  std::string global_config_path;
  std::string inet_addr;
  std::string db_event_fifo_path;
  std::string mch_artifact;
  std::string mch_capture_dir;
  bool mch_classify_only = false;
  
  td::OptionParser p;
  p.set_description("Emulate TON traces");
  p.add_option('\0', "help", "prints_help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_option('D', "db", "Path to TON DB folder", [&](td::Slice fname) { 
    db_root = fname.str();
  });
  p.add_option('W', "working-dir", "Path to index working dir for secondary rocksdb logs", [&](td::Slice fname) { 
    working_dir = fname.str();
  });
  p.add_option('\0', "testnet", "Use for testnet. It is used for correct detecting of .ton DNS entries (in testnet .ton collection has a different address)", [&]() {
    NftItemDetectorR::is_testnet = true;
  });

  p.add_checked_option('t', "threads", "Scheduler threads (default: 7)", [&](td::Slice fname) { 
    int v;
    try {
      v = std::stoi(fname.str());
    } catch (...) {
      return td::Status::Error(ton::ErrorCode::error, "bad value for --threads: not a number");
    }
    threads = v;
    return td::Status::OK();
  });

  p.add_option('\0', "redis", "Redis URI (default: 'tcp://127.0.0.1:6379')", [&](td::Slice fname) { 
    redis_dsn = fname.str();
  });

  p.add_option('\0', "redis-channel", "Redis channel name for input msgs", [&](td::Slice fname) { 
    redis_channel = fname.str();
  });

  p.add_option('\0', "global-config", "Path to global config json file (for listening overlay)", [&](td::Slice fname) { 
    global_config_path = fname.str();
  });

  p.add_option('\0', "addr", "ip:port of this machine (for listening overlay)", [&](td::Slice fname) { 
    inet_addr = fname.str();
  });

  p.add_option('\0', "db-event-fifo", "Path to FIFO pipe for DB events", [&](td::Slice fname) { 
    db_event_fifo_path = fname.str();
  });

  p.add_option('\0', "mch-artifact", "Path to the MCH IR artifact; enables in-process shadow classification (log-only)", [&](td::Slice fname) {
    mch_artifact = fname.str();
  });

  p.add_option('\0', "mch-capture", "Directory to dump shadow classifier inputs as JSON (requires --mch-artifact)", [&](td::Slice fname) {
    mch_capture_dir = fname.str();
  });

  p.add_option('\0', "mch-classify-only", "Do not write emulated traces to Redis (requires --mch-artifact)", [&]() {
    mch_classify_only = true;
  });


  auto S = p.run(argc, argv);
  if (S.is_error()) {
    LOG(ERROR) << "failed to parse options: " << S.move_as_error();
    std::_Exit(2);
  }

  if (db_root.size() == 0) {
    std::cerr << "'--db' option missing" << std::endl;
    std::_Exit(2);
  }

  if (working_dir.size() == 0) {
    working_dir = PSTRING() << "/tmp/index_worker_" << getpid();
    LOG(WARNING) << "Working dir not specified, using " << working_dir;
  }

  if (global_config_path.empty() ^ inet_addr.empty()) {
    std::cerr << "'--global-config' must be present with '--addr'" << std::endl;
    std::_Exit(2);
  }

  if (mch_artifact.empty() && (!mch_capture_dir.empty() || mch_classify_only)) {
    std::cerr << "'--mch-capture' and '--mch-classify-only' require '--mch-artifact'" << std::endl;
    std::_Exit(2);
  }

  mch::EmuShadowConfig mch_shadow;
  if (!mch_artifact.empty()) {
    // The artifact and the binary ship together: a mismatch is a build bug, not
    // a reason to run with shadow classification silently skipped.
    auto r_prep = mch::load_engine_prep(mch_artifact);
    if (r_prep.is_error()) {
      LOG(FATAL) << "failed to load MCH artifact '" << mch_artifact << "': " << r_prep.move_as_error();
    }
    mch_shadow.prep = r_prep.move_as_ok();
    mch_shadow.capture_dir = mch_capture_dir;
    mch_shadow.pending = std::make_shared<std::atomic<std::size_t>>(0);
    LOG(INFO) << "MCH emu shadow ENABLED (artifact " << mch_artifact << ")"
              << (mch_capture_dir.empty() ? "" : ", capture dir " + mch_capture_dir)
              << (mch_classify_only ? ", classify-only (no Redis writes)" : "");
  }

  td::actor::Scheduler scheduler({threads});
  td::actor::ActorOwn<DbScanner> db_scanner;
  td::actor::ActorOwn<ITraceInsertManager> insert_manager;

  scheduler.run_in_context([&] { 
    db_scanner = td::actor::create_actor<DbScanner>("scanner", db_root, dbs_secondary, working_dir, 0.05f);
    if (mch_classify_only) {
      insert_manager = td::actor::create_actor<NullTraceInsertManager>("NullTraceInsertManager");
    } else {
      insert_manager = td::actor::create_actor<RedisInsertManager>("RedisInsertManager", redis_dsn);
    }
    td::actor::create_actor<TraceEmulatorScheduler>("integritychecker", db_scanner.get(), insert_manager.get(),
      global_config_path, inet_addr, redis_dsn, redis_channel, working_dir, db_event_fifo_path, mch_shadow).release();
  });
  
  scheduler.run();

  return 0;
}
