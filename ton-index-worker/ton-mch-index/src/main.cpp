#include "td/utils/port/signals.h"
#include "td/utils/OptionParser.h"
#include "td/utils/logging.h"
#include "td/utils/check.h"

#include "crypto/vm/cp0.h"

#include <cstdint>
#include <string>

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler().ensure();
  CHECK(vm::init_op_cp0());

  std::string db_root;
  std::string workdir;
  std::string out_path;
  std::uint32_t seqno_from = 0;
  std::uint32_t seqno_to = 0;
  std::uint32_t prewarm = 50;
  std::int32_t threads = 7;
  bool no_tier2 = false;

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
  p.set_description("mch batch classifier over a node RocksDB seqno range");
  p.add_option('D', "db", "node database root", [&](td::Slice v) { db_root = v.str(); });
  p.add_option('W', "workdir", "private secondary-mode workdir", [&](td::Slice v) { workdir = v.str(); });
  p.add_option('o', "out", "output JSONL path", [&](td::Slice v) { out_path = v.str(); });
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
               [&](td::Slice) { no_tier2 = true; });

  auto status = p.run(argc, argv);
  if (status.is_error()) {
    LOG(ERROR) << status.error().message();
    return 1;
  }
  if (db_root.empty() || out_path.empty() || seqno_from == 0 || seqno_to < seqno_from) {
    LOG(ERROR) << "required: --db, --out, --from, --to (with to >= from)";
    return 1;
  }

  LOG(INFO) << "ton-mch-index: db=" << db_root << " range=[" << seqno_from << "," << seqno_to
            << "] prewarm=" << prewarm << " threads=" << threads << " out=" << out_path
            << " tier2=" << (no_tier2 ? "off" : "on");
  LOG(ERROR) << "pipeline not wired yet (scaffold)";
  return 2;
}
