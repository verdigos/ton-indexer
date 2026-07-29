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
#include "emu/EmuLookupActor.h"  // tier-2 pool + lookup_pool_fetch (§9)

#include <vector>


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
  bool mch_classify_nonblocking = false;
  td::uint32 mch_stress_backfill = 0;
  std::int64_t mch_classify_deadline_ms = mch::kClassifyDeadlineUs / 1000;
  std::size_t mch_work_cap = mch::kMaxClassifyNodes;
  kvrocks_state::KvrocksConfig mch_kvrocks_config;
  ActionWriteConfig action_cfg;
  bool mch_actions_only = false;

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

  p.add_option('\0', "mch-classify-only", "Do not write emulated traces to Redis; without '--mch-artifact' this is a pure-emulator run with no classifier", [&]() {
    mch_classify_only = true;
  });

  p.add_option('\0', "mch-classify-nonblocking", "Insert traces without waiting for shadow classification (requires --mch-artifact)", [&]() {
    mch_classify_nonblocking = true;
  });

  p.add_checked_option('\0', "mch-stress-backfill", "TEST ONLY: start N masterchain blocks behind the head, emulating them as one catch-up burst (default: 0)", [&](td::Slice fname) {
    int v;
    try {
      v = std::stoi(fname.str());
    } catch (...) {
      return td::Status::Error(ton::ErrorCode::error, "bad value for --mch-stress-backfill: not a number");
    }
    if (v < 0) {
      return td::Status::Error(ton::ErrorCode::error, "bad value for --mch-stress-backfill: must not be negative");
    }
    mch_stress_backfill = v;
    return td::Status::OK();
  });

  p.add_checked_option('\0', "mch-classify-deadline-ms", "TEST ONLY: shadow classify queue deadline in ms (default: 200, requires --mch-artifact)", [&](td::Slice fname) {
    int v;
    try {
      v = std::stoi(fname.str());
    } catch (...) {
      return td::Status::Error(ton::ErrorCode::error, "bad value for --mch-classify-deadline-ms: not a number");
    }
    if (v <= 0) {
      return td::Status::Error(ton::ErrorCode::error, "bad value for --mch-classify-deadline-ms: must be positive");
    }
    mch_classify_deadline_ms = v;
    return td::Status::OK();
  });


  p.add_checked_option('\0', "mch-work-cap", "Refuse shadow classification for traces above N transactions (default: 64, em2-writeback-design.md §5; requires --mch-artifact)", [&](td::Slice fname) {
    int v;
    try {
      v = std::stoi(fname.str());
    } catch (...) {
      return td::Status::Error(ton::ErrorCode::error, "bad value for --mch-work-cap: not a number");
    }
    if (v <= 0) {
      return td::Status::Error(ton::ErrorCode::error, "bad value for --mch-work-cap: must be positive");
    }
    mch_work_cap = static_cast<std::size_t>(v);
    return td::Status::OK();
  });

  // --- tier-2 lookups (em2-tier2-design.md §9). Absent => no lookup actors, the
  // hook is unset, the fixpoint commits round 0, and the emission is
  // byte-identical to EM1.5.
  p.add_option('\0', "mch-kvrocks", "Kvrocks direct Redis URI for tier-2 lookups, e.g. tcp://127.0.0.1:6666/0 (requires --mch-artifact)", [&](td::Slice value) {
    mch_kvrocks_config.enabled = true;
    mch_kvrocks_config.uri = value.str();
  });
  p.add_checked_option('\0', "mch-kvrocks-sentinels", "Comma-separated Kvrocks Sentinel nodes for tier-2 lookups", [&](td::Slice value) {
    try {
      mch_kvrocks_config.enabled = true;
      mch_kvrocks_config.sentinel_nodes = kvrocks_state::parse_kvrocks_sentinel_nodes(value.str());
    } catch (const std::exception &e) {
      return td::Status::Error(ton::ErrorCode::error, PSLICE() << "bad value for --mch-kvrocks-sentinels: " << e.what());
    }
    return td::Status::OK();
  });
  p.add_option('\0', "mch-kvrocks-master", "Kvrocks Sentinel master name (required with --mch-kvrocks-sentinels)", [&](td::Slice value) {
    mch_kvrocks_config.sentinel_master_name = value.str();
  });
  p.add_option('\0', "mch-kvrocks-user", "Kvrocks username", [&](td::Slice value) {
    mch_kvrocks_config.user = value.str();
  });
  p.add_option('\0', "mch-kvrocks-password", "Kvrocks password", [&](td::Slice value) {
    mch_kvrocks_config.password = value.str();
  });
  p.add_checked_option('\0', "mch-kvrocks-db", "Kvrocks logical database number", [&](td::Slice value) {
    int v;
    try {
      v = std::stoi(value.str());
    } catch (...) {
      return td::Status::Error(ton::ErrorCode::error, "bad value for --mch-kvrocks-db: not a number");
    }
    if (v < 0) {
      return td::Status::Error(ton::ErrorCode::error, "bad value for --mch-kvrocks-db: must be non-negative");
    }
    mch_kvrocks_config.db = v;
    return td::Status::OK();
  });

  // --- EM2 §8 phase-2 A/B. Both key names default to PRODUCTION, so a run
  // without these flags writes exactly what Python writes today.
  p.add_option('\0', "mch-actions-field", "Trace-hash field for the write-back blob (default: 'actions'; phase-2 A/B uses 'actions_mch')", [&](td::Slice value) {
    action_cfg.actions_field = value.str();
  });
  p.add_option('\0', "mch-aai-prefix", "Key prefix for the per-account action index (default: '_aai:'; phase-2 A/B uses '_aai_mch:')", [&](td::Slice value) {
    action_cfg.aai_prefix = value.str();
  });
  p.add_option('\0', "mch-actions-only", "SHADOW WRITE: write ONLY the write-back keys into an EXISTING trace hash, publish nothing (em2-writeback-design.md §8 phase 2; requires --mch-artifact)", [&]() {
    mch_actions_only = true;
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

  if (mch_artifact.empty() && (!mch_capture_dir.empty() || mch_classify_nonblocking ||
                               mch_classify_deadline_ms != mch::kClassifyDeadlineUs / 1000 ||
                               mch_work_cap != mch::kMaxClassifyNodes ||
                               mch_kvrocks_config.enabled || mch_actions_only)) {
    std::cerr << "'--mch-capture', '--mch-classify-nonblocking', '--mch-classify-deadline-ms', "
                 "'--mch-work-cap', '--mch-kvrocks*' and '--mch-actions-only' "
                 "require '--mch-artifact'" << std::endl;
    std::_Exit(2);
  }

  // Mirrors ton-index-postgres/src/main.cpp:521-527.
  if (mch_kvrocks_config.enabled) {
    if (mch_kvrocks_config.use_sentinel() && !mch_kvrocks_config.uri.empty()) {
      std::cerr << "Use either --mch-kvrocks or --mch-kvrocks-sentinels, not both" << std::endl;
      std::_Exit(2);
    }
    if (mch_kvrocks_config.use_sentinel() && mch_kvrocks_config.sentinel_master_name.empty()) {
      std::cerr << "'--mch-kvrocks-master' is required with '--mch-kvrocks-sentinels'" << std::endl;
      std::_Exit(2);
    }
  }

  // --mch-actions-only writes ONLY the shadow keys, so it must not also be told
  // to skip writing (nothing would be written) and it must be pointed at
  // non-production field names — otherwise a "shadow" run overwrites the very
  // field production Python owns.
  if (mch_actions_only) {
    if (mch_classify_only) {
      std::cerr << "'--mch-actions-only' and '--mch-classify-only' are mutually exclusive"
                << std::endl;
      std::_Exit(2);
    }
    if (action_cfg.actions_field == "actions" || action_cfg.aai_prefix == "_aai:") {
      std::cerr << "'--mch-actions-only' is a SHADOW write: set '--mch-actions-field' and "
                   "'--mch-aai-prefix' to non-production names (e.g. actions_mch / _aai_mch:)"
                << std::endl;
      std::_Exit(2);
    }
  }

  if (mch_classify_only && mch_artifact.empty()) {
    LOG(INFO) << "classify-only without --mch-artifact: emulating only, no Redis writes, no classification";
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
    mch_shadow.nonblocking = mch_classify_nonblocking;
    mch_shadow.gate = std::make_shared<mch::EmuGate>();
    mch_shadow.gate->classify_deadline_us = mch_classify_deadline_ms * 1000;
    mch_shadow.max_nodes = mch_work_cap;
    LOG(INFO) << "MCH emu shadow ENABLED (artifact " << mch_artifact << ")"
              << (mch_capture_dir.empty() ? "" : ", capture dir " + mch_capture_dir)
              << (mch_classify_only ? ", classify-only (no Redis writes)" : "")
              << (mch_classify_nonblocking ? ", nonblocking (insert does not wait)"
                                           : ", insert gated on classification")
              << ", classify deadline " << mch_classify_deadline_ms << " ms"
              << ", work cap " << mch_work_cap << " nodes"
              << ", actions field '" << action_cfg.actions_field << "', aai prefix '"
              << action_cfg.aai_prefix << "'"
              << (mch_actions_only ? " (SHADOW: actions only, no publishes)" : "");
  }

  td::actor::Scheduler scheduler({threads});
  td::actor::ActorOwn<DbScanner> db_scanner;
  td::actor::ActorOwn<ITraceInsertManager> insert_manager;
  // Must outlive the classifier, which holds the fetch functor that addresses
  // them (EmuLookupActor.h). Declared here so their lifetime is main()'s.
  std::vector<td::actor::ActorOwn<mch::EmuLookupActor>> mch_lookup_pool;

  scheduler.run_in_context([&] {
    db_scanner = td::actor::create_actor<DbScanner>("scanner", db_root, dbs_secondary, working_dir, 0.05f);
    if (mch_classify_only) {
      insert_manager = td::actor::create_actor<NullTraceInsertManager>("NullTraceInsertManager");
    } else if (mch_actions_only) {
      insert_manager = td::actor::create_actor<ActionsOnlyInsertManager>("ActionsOnlyInsertManager", redis_dsn, action_cfg);
    } else {
      insert_manager = td::actor::create_actor<RedisInsertManager>("RedisInsertManager", redis_dsn, action_cfg);
    }
    // Tier 2, if configured. A POOL because each actor BLOCKS one scheduler
    // thread for a round trip (EmuLookupActor.h) — round-robining keeps one slow
    // store round from serializing every suspended trace behind it. Built before
    // the scheduler actor, whose ctor copies mch_shadow into the classifier.
    if (mch_kvrocks_config.enabled) {
      std::vector<td::actor::ActorId<mch::EmuLookupActor>> pool;
      for (std::size_t i = 0; i < mch::kLookupActors; i++) {
        mch_lookup_pool.push_back(td::actor::create_actor<mch::EmuLookupActor>(
            PSTRING() << "MchLookup" << i, mch_kvrocks_config));
        pool.push_back(mch_lookup_pool.back().get());
      }
      mch_shadow.tier2_fetch = mch::lookup_pool_fetch(std::move(pool));
      // HOST ONLY — never the credentials (§9).
      LOG(INFO) << "[mch-emu] tier-2 ENABLED (" << mch_kvrocks_config.describe() << ", "
                << mch::kLookupActors << " lookup actors)";
    }
    td::actor::create_actor<TraceEmulatorScheduler>("integritychecker", db_scanner.get(), insert_manager.get(),
      global_config_path, inet_addr, redis_dsn, redis_channel, working_dir, db_event_fifo_path, mch_shadow,
      mch_stress_backfill).release();
  });
  
  scheduler.run();

  return 0;
}
