#include "td/utils/port/signals.h"
#include "td/utils/OptionParser.h"
#include "td/utils/format.h"
#include "td/utils/logging.h"
#include "td/utils/check.h"

#include "crypto/vm/cp0.h"

#include "DbScanner.h"
#include "TraceTaskScheduler.h"
#include "TaskResultInserter.h"
#include "GenMatchers.h"

#include <algorithm>


int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler().ensure();

  CHECK(vm::init_op_cp0());

  // options
  std::string db_root;
  std::string working_dir;
  td::uint32 threads = 7;
  std::string redis_dsn = "tcp://127.0.0.1:6379";
  std::string redis_queue_name = "emulatorqueue";
  bool mch_disable = false;
  bool mch_no_tier2 = false;
  int mch_workers = 1;
  
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

  p.add_option('\0', "redis-queue", "Redis queue name for input msgs", [&](td::Slice fname) { 
    redis_queue_name = fname.str();
  });

  p.add_option('\0', "mch-disable", "Disable in-process MCH classification", [&]() {
    mch_disable = true;
  });

  p.add_option('\0', "mch-no-tier2", "Disable celldb tier-2 lookups (tier-1-only classification)", [&]() {
    mch_no_tier2 = true;
  });

  p.add_checked_option('\0', "mch-workers", "MCH classifier workers (default: 1)", [&](td::Slice value) {
    int v;
    try {
      v = std::stoi(value.str());
    } catch (...) {
      return td::Status::Error(ton::ErrorCode::error, "bad value for --mch-workers: not a number");
    }
    mch_workers = std::clamp(v, 1, 64);
    return td::Status::OK();
  });

  auto S = p.run(argc, argv);
  if (S.is_error()) {
    LOG(ERROR) << "failed to parse options: " << S.move_as_error();
    std::_Exit(2);
  }

  if (db_root.size() == 0) {
    LOG(ERROR) << "'--db' option missing";
    std::_Exit(2);
  }

  if (working_dir.size() == 0) {
    working_dir = PSTRING() << "/tmp/index_worker_" << getpid();
    LOG(WARNING) << "Working dir not specified, using " << working_dir;
  }
  if (redis_queue_name.size() == 0) {
    LOG(ERROR) << "'--redis-queue' option missing";
    std::_Exit(2);
  }
  if (redis_dsn.size() == 0) {
    LOG(ERROR) << "'--redis' option missing";
    std::_Exit(2);
  }

  mch::EmuClassifierConfig mch_classifier_config;
  mch_classifier_config.workers = mch_workers;
  if (mch_disable) {
    LOG(WARNING) << "MCH classification DISABLED (--mch-disable): with_actions tasks get no actions";
  } else {
    auto r_prep = mch::make_engine_prep();
    if (r_prep.is_error()) {
      LOG(FATAL) << "MCH engine prep failed: " << r_prep.move_as_error();
    }
    mch_classifier_config.prep = r_prep.move_as_ok();
    mch_classifier_config.tier2 = !mch_no_tier2;
    LOG(INFO) << "MCH classification ENABLED (artifact sha " << mch::gen_matchers_ir_source_sha()
              << "), celldb tier-2 " << (mch_classifier_config.tier2 ? "ON" : "OFF")
              << " workers=" << mch_classifier_config.workers;
  }

  td::actor::Scheduler scheduler({threads});
  td::actor::ActorOwn<DbScanner> db_scanner;
  td::actor::ActorOwn<ITaskResultInserter> insert_manager;

  scheduler.run_in_context([&] { 
    db_scanner = td::actor::create_actor<DbScanner>("scanner", db_root, dbs_secondary, working_dir, 0.5);
    insert_manager = td::actor::create_actor<RedisTaskResultInsertManager>("OnDemandRedisInsertManager", redis_dsn);
    td::actor::create_actor<TraceTaskScheduler>("integritychecker", db_scanner.get(), insert_manager.get(), redis_dsn, redis_queue_name, working_dir,
                                                  std::move(mch_classifier_config)).release();
  });
  
  scheduler.run();

  return 0;
}
