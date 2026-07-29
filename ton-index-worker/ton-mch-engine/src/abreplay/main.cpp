// mch-ab-replay — EM2 phase-1 A/B replay adapter (em2-writeback-design.md §8,
// wave W7 item iii).
//
// Phase 1 answers one question offline: for the traces prod Redis is holding
// right now, what would OUR writer have put in the `actions` field, and how does
// it differ from what Python actually put there? This binary is the "ours" half.
// It reads a read-only HGETALL sweep (JSONL, format pinned in twins/README.md),
// rebuilds each trace from the stored node blobs, and runs the SAME classify and
// serialize code the emulator will run in production — `classify_trace_fixpoint`
// (ClassifyDriver.h) and `serialize_actions` (emu/EmuActionSerialize.h), neither
// of which is reimplemented here.
//
// Output is the `--actions-msgpack` line format, so twins/ab_replay_diff.py
// consumes it with no new comparator code:
//
//     === <trace_key>
//     ACTION anchor=<block_key> <base64 msgpack>
//
// NO REDIS, NO NETWORK, NO DB. The input is a file; producing it from prod is a
// separate read-only sweep.
//
//   mch-ab-replay --make-dump -T <traces dir> -o dump.jsonl   # dry-run input
//   mch-ab-replay --replay dump.jsonl -I <ir> [--twin-view]
//   mch-ab-replay --replay dump.jsonl -I <ir> --fixture-lookups <traces dir>
#include "ActionBuild.h"
#include "BlockTree.h"
#include "BuildDriver.h"
#include "BuildRuntime.h"
#include "ClassifyCore.h"
#include "ClassifyDriver.h"
#include "IrLoader.h"
#include "SchemaTraceLoader.h"
#include "TraceLoader.h"
#include "abreplay/RedisReplay.h"
#include "emu/EmuActionSerialize.h"
#include "emu/EmuTypes.h"

#include "td/utils/JsonBuilder.h"
#include "td/utils/OptionParser.h"
#include "td/utils/base64.h"
#include "td/utils/logging.h"
#include "td/utils/port/signals.h"

#include "crypto/vm/cp0.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mch {

namespace {

namespace fs = std::filesystem;

// --- JSONL --------------------------------------------------------------------

std::string json_escape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

void write_dump_line(std::ostream &out, const RedisTraceDump &d) {
  out << "{\"trace_key\":\"" << json_escape(d.trace_key) << "\",\"trace_id\":\""
      << json_escape(d.trace_id) << "\",\"root_node\":\"" << json_escape(d.root_node)
      << "\",\"nodes\":{";
  bool first = true;
  for (const auto &[key, blob] : d.nodes) {
    if (!first) out << ",";
    first = false;
    out << "\"" << json_escape(key) << "\":\"" << td::base64_encode(blob) << "\"";
  }
  out << "},\"interfaces\":{";
  first = true;
  for (const auto &[acc, blob] : d.interfaces) {
    if (!first) out << ",";
    first = false;
    out << "\"" << json_escape(acc) << "\":\"" << td::base64_encode(blob) << "\"";
  }
  out << "}}\n";
}

// One JSONL record -> RedisTraceDump. `actions_b64` (Python's side) is present in
// the same file and deliberately IGNORED here: ab_replay_diff.py reads it, we do
// not, so the two halves of the comparison can never contaminate each other.
td::Result<RedisTraceDump> parse_dump_line(std::string line) {
  TRY_RESULT(json, td::json_decode(td::MutableSlice(line)));
  if (json.type() != td::JsonValue::Type::Object) {
    return td::Status::Error("not a JSON object");
  }
  td::JsonObject &obj = json.get_object();
  RedisTraceDump d;
  TRY_RESULT_ASSIGN(d.trace_key, obj.get_required_string_field("trace_key"));
  TRY_RESULT_ASSIGN(d.root_node, obj.get_optional_string_field("root_node"));
  TRY_RESULT_ASSIGN(d.trace_id, obj.get_optional_string_field("trace_id"));

  auto load_section = [&obj](td::Slice name, std::map<std::string, std::string> &into,
                             bool required) -> td::Status {
    if (!obj.has_field(name)) {
      if (required) {
        return td::Status::Error(PSLICE() << "missing `" << name << "` — the sweep captured only "
                                          << "the comparator's two fields and cannot be replayed "
                                          << "(twins/README.md, extended dump format)");
      }
      return td::Status::OK();
    }
    TRY_RESULT(section, obj.extract_required_field(name, td::JsonValue::Type::Object));
    td::Status st = td::Status::OK();
    section.get_object().foreach ([&](td::Slice key, const td::JsonValue &value) {
      if (st.is_error()) return;
      if (value.type() != td::JsonValue::Type::String) {
        st = td::Status::Error(PSLICE() << name << "." << key << " is not a base64 string");
        return;
      }
      auto raw = td::base64_decode(value.get_string());
      if (raw.is_error()) {
        st = raw.move_as_error_prefix(PSLICE() << name << "." << key << ": ");
        return;
      }
      into.emplace(key.str(), raw.move_as_ok());
    });
    return st;
  };
  TRY_STATUS(load_section("nodes", d.nodes, true));
  TRY_STATUS(load_section("interfaces", d.interfaces, false));
  return d;
}

// --- the emission view --------------------------------------------------------
//
// serialize_actions reads exactly two things off the view: `trace_id` (the
// `trace_external_hash_norm` wire key — Python's pendings.py:239 writes the Redis
// trace key there) and the min over node finalities (the `finality` key). §2.5.
EmuTraceView view_for(const std::string &trace_key, const std::vector<std::uint8_t> &finality) {
  EmuTraceView v;
  v.trace_id = trace_key;
  for (std::uint8_t f : finality) {
    EmuTxRef node;
    node.finality = static_cast<EmuFinality>(f);
    v.nodes.push_back(node);
  }
  return v;
}

// The stub emission `--actions-msgpack` serializes against (Classify.cpp's
// twin_view / twins/py_actions_msgpack.py's _STUB_TRACE). A fixture is not an
// emission, so the two emission-derived keys are sentinels on BOTH sides; the
// corpus round-trip has to use the same sentinels or it would diff on them and
// on nothing else.
EmuTraceView twin_view() {
  EmuTraceView v;
  v.trace_id = "twin";
  EmuTxRef node;
  node.finality = EmuFinality::finalized;
  v.nodes.push_back(node);
  return v;
}

// --- fixture enumeration (shared by --make-dump and --fixture-lookups) --------

std::vector<std::string> collect_fixtures(const std::vector<std::string> &paths) {
  std::vector<std::string> out;
  for (const std::string &p : paths) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
      std::vector<std::string> names;
      for (const auto &e : fs::directory_iterator(p)) {
        if (e.path().extension() == ".lz4") {
          names.push_back(e.path().string());
        }
      }
      std::sort(names.begin(), names.end());
      out.insert(out.end(), names.begin(), names.end());
    } else {
      out.push_back(p);
    }
  }
  return out;
}

int run_make_dump(const std::vector<std::string> &trace_paths, const std::string &out_path) {
  std::vector<std::string> fixtures = collect_fixtures(trace_paths);
  if (fixtures.empty()) {
    std::fprintf(stderr, "no fixtures under -T\n");
    return 2;
  }
  std::ofstream file;
  std::ostream *out = &std::cout;
  if (!out_path.empty()) {
    file.open(out_path, std::ios::binary);
    if (!file) {
      std::fprintf(stderr, "cannot open %s for writing\n", out_path.c_str());
      return 2;
    }
    out = &file;
  }
  std::size_t nodes = 0, ifaces = 0;
  for (const std::string &f : fixtures) {
    auto r_trace = load_trace(f);
    if (r_trace.is_error()) {
      std::fprintf(stderr, "%s: %s\n", f.c_str(), r_trace.error().message().c_str());
      return 2;
    }
    Trace trace = r_trace.move_as_ok();
    // The join key is the FIXTURE FILENAME, matching how mk_hset_dump.py keys
    // Python's side (it reads the `=== <name>` header of the twin dump) and how
    // `--actions-msgpack` labels its traces. A prod sweep uses the real key.
    auto r_dump = fixture_to_redis_dump(fs::path(f).filename().string(), trace);
    if (r_dump.is_error()) {
      std::fprintf(stderr, "%s: %s\n", f.c_str(), r_dump.error().message().c_str());
      return 2;
    }
    RedisTraceDump d = r_dump.move_as_ok();
    nodes += d.nodes.size();
    ifaces += d.interfaces.size();
    write_dump_line(*out, d);
  }
  out->flush();
  std::fprintf(stderr, "# %zu traces, %zu nodes, %zu interface blobs\n", fixtures.size(), nodes,
               ifaces);
  return 0;
}

// --- replay -------------------------------------------------------------------

// Harness-only lookup override (--fixture-lookups). Transactions still come from
// the Redis blobs — the code under test — while the LOOKUP data comes from the
// fixture, exactly as `--actions-msgpack` serves it. That isolates the converter
// from the interface-capability question: AddressInterfaces carries only the four
// kinds the emulator detects, so dedust_pool / nominator_pool / jvault_assets are
// permanent replay misses and would otherwise mask a converter defect.
struct FixtureIfaces {
  std::map<std::string, std::map<std::string, Value>> by_trace_key;
};

td::Result<FixtureIfaces> load_fixture_interfaces(const std::vector<std::string> &paths) {
  FixtureIfaces out;
  for (const std::string &f : collect_fixtures(paths)) {
    TRY_RESULT(trace, load_trace(f));
    out.by_trace_key.emplace(fs::path(f).filename().string(), std::move(trace.interfaces));
  }
  return out;
}

int run_replay(const std::string &ir_path, const std::string &dump_path, bool twin_view_mode,
               const std::vector<std::string> &fixture_lookup_paths) {
  auto r_ir = load_ir(ir_path);
  if (r_ir.is_error()) {
    std::fprintf(stderr, "failed to load IR artifact: %s\n", r_ir.error().message().c_str());
    return 2;
  }
  LoadedIr ir = r_ir.move_as_ok();
  warn_missing_artifact_parsers(ir);
  ClassifySetup setup = prepare_classify(ir);
  if (setup.table_missing || setup.fn_missing) {
    std::fprintf(stderr, "%s\n", setup.error.c_str());
    return 2;
  }
  // SKIP lines first, byte-identical to every other dump mode.
  for (const auto &s : setup.skips) {
    std::printf("SKIP %s reason=%s\n", s.first.c_str(), s.second.c_str());
  }

  FixtureIfaces fixture_ifaces;
  if (!fixture_lookup_paths.empty()) {
    auto r = load_fixture_interfaces(fixture_lookup_paths);
    if (r.is_error()) {
      std::fprintf(stderr, "--fixture-lookups: %s\n", r.error().message().c_str());
      return 2;
    }
    fixture_ifaces = r.move_as_ok();
  }

  std::ifstream in(dump_path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", dump_path.c_str());
    return 2;
  }

  std::size_t traces = 0, rows = 0, failed = 0;
  std::size_t unreachable = 0, nodes_bad = 0, iface_bad = 0, iface_dropped = 0;
  ActionSerializeStats ser_stats;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    auto r_dump = parse_dump_line(line);
    if (r_dump.is_error()) {
      std::fprintf(stderr, "bad dump line: %s\n", r_dump.error().message().c_str());
      return 2;
    }
    RedisTraceDump dump = r_dump.move_as_ok();
    traces++;
    std::printf("=== %s\n", dump.trace_key.c_str());

    auto r_replay = redis_dump_to_replay(dump);
    if (r_replay.is_error()) {
      std::printf("ERROR: %s\n", r_replay.error().message().c_str());
      failed++;
      continue;
    }
    RedisReplayTrace rt = r_replay.move_as_ok();
    unreachable += rt.unreachable_nodes;
    nodes_bad += rt.nodes_undecodable;
    iface_bad += rt.iface_blobs_undecodable;
    iface_dropped += rt.iface_variants_dropped;

    EmuTraceView view = twin_view_mode ? twin_view() : view_for(dump.trace_key, rt.node_finality);
    // In Redis these are the same string; only the dry run splits them (see
    // RedisTraceDump::trace_id).
    const std::string &trace_id = dump.trace_id.empty() ? dump.trace_key : dump.trace_id;

    // THE EXISTING PATH. Nothing between the converter and the writer is local
    // to this tool: schema_to_trace + to_tree + init_block + the classify
    // fixpoint all live behind classify_trace_fixpoint, and the payload bytes
    // come out of serialize_actions.
    DriverResult dr = [&] {
      if (fixture_lookup_paths.empty()) {
        return classify_trace_fixpoint(trace_id, rt.txs, rt.ifaces, ir, setup);
      }
      // Same builders, one substitution: FixtureLookupSource over the fixture's
      // own interface dict, which is what run_dump (Classify.cpp) uses.
      auto it = fixture_ifaces.by_trace_key.find(dump.trace_key);
      static const std::map<std::string, Value> kEmpty;
      const std::map<std::string, Value> *ifaces =
          it != fixture_ifaces.by_trace_key.end() ? &it->second : &kEmpty;
      auto make_ctx = [&]() -> td::Result<std::unique_ptr<TraceContext>> {
        TRY_RESULT(trace, schema_to_trace(trace_id, rt.txs));
        auto ctx = std::make_unique<TraceContext>();
        ctx->trace = std::move(trace);
        ctx->tree = to_tree(ctx->trace);
        if (ctx->tree.root == nullptr) {
          return td::Status::Error("empty event tree");
        }
        ctx->root = init_block(ctx->arena, ctx->tree.root);
        return ctx;
      };
      auto make_src = [&](TraceContext &, ParsedBlockLookupSource::Tier2Hook) {
        return RoundSource{std::make_shared<FixtureLookupSource>(ifaces), nullptr};
      };
      return run_classify_fixpoint(make_ctx, make_src, ir, setup);
    }();

    if (dr.malformed) {
      std::printf("ERROR: %s\n", dr.error.c_str());
      failed++;
      continue;
    }
    if (dr.ctx->tree.unlinked != 0) {
      std::fprintf(stderr, "WARN %s: %zu orphan nodes unreachable from root\n",
                   dump.trace_key.c_str(), dr.ctx->tree.unlinked);
    }
    auto emit = [&](const char *tag, const std::string &anchor, const Action &act) {
      std::printf("%s anchor=%s %s\n", tag, anchor.c_str(),
                  td::base64_encode(serialize_actions({act}, view, &ser_stats)).c_str());
      rows++;
    };

    const ClassifyResult &res = dr.res;
    if (res.failure) {
      std::printf("TRACE_FAIL\n");
      std::fprintf(stderr, "TRACE_FAIL %s: %s\n", dump.trace_key.c_str(),
                   res.failure_reason.c_str());
      for (const CoreAction &a : res.fallback_actions) {
        Action act;
        if (!build_action(a.produced, act)) continue;
        emit("FALLBACK", block_key(a.anchor), act);
      }
      continue;
    }
    for (const CoreAction &a : res.actions) {
      Action act;
      if (!build_action(a.produced, act)) continue;  // btype outside the ported set
      emit("ACTION", block_key(a.anchor), act);
    }
    if (res.unknown_trace) {
      emit("ACTION", "unknown", create_unknown_action(dr.ctx->trace));
    }
  }

  std::fprintf(stderr,
               "# replay traces=%zu rows=%zu failed=%zu | unreachable_nodes=%zu "
               "nodes_undecodable=%zu iface_blobs_undecodable=%zu iface_variants_dropped=%zu | "
               "ser{float=%zu,cell=%zu,unrenderable=%zu}\n",
               traces, rows, failed, unreachable, nodes_bad, iface_bad, iface_dropped,
               ser_stats.float_values, ser_stats.cell_values, ser_stats.unrenderable);
  return 0;
}

}  // namespace
}  // namespace mch

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_WARNING);
  td::set_default_failure_signal_handler().ensure();
  vm::init_op_cp0();

  std::string ir_path;
  std::string dump_path;
  std::string out_path;
  bool make_dump = false;
  bool twin_view_mode = false;
  std::vector<std::string> trace_paths;
  std::vector<std::string> fixture_lookup_paths;

  td::OptionParser p;
  p.set_description("EM2 phase-1 A/B replay adapter (offline; no Redis, no DB)");
  p.add_option('\0', "replay",
               "Replay a trace HSET dump (JSONL, twins/README.md format) and emit "
               "`--actions-msgpack` lines: --replay <dump.jsonl> -I <artifact>",
               [&](td::Slice value) { dump_path = value.str(); });
  p.add_option('\0', "make-dump",
               "DRY RUN: manufacture an extended JSONL dump from the fixture corpus, packed "
               "with the emulator's own Serializer.hpp structs: -T <fixture|dir> [-o out]",
               [&]() { make_dump = true; });
  p.add_option('\0', "twin-view",
               "Serialize against the `--actions-msgpack` stub emission (trace_external_hash_norm"
               "=\"twin\", finality=finalized) instead of the dump's real values — required for "
               "the corpus round-trip, wrong for a prod sweep",
               [&]() { twin_view_mode = true; });
  p.add_option('\0', "fixture-lookups",
               "HARNESS: serve lookups from the fixture corpus instead of the dump's interface "
               "blobs, isolating the transaction converter from the interface-capability gap",
               [&](td::Slice value) { fixture_lookup_paths.push_back(value.str()); });
  p.add_option('I', "ir", "Path to the MCH IR artifact (json)",
               [&](td::Slice value) { ir_path = value.str(); });
  p.add_option('T', "trace", "Path to an .lz4 trace fixture or a directory (repeatable)",
               [&](td::Slice value) { trace_paths.push_back(value.str()); });
  p.add_option('o', "out", "Write --make-dump output here instead of stdout",
               [&](td::Slice value) { out_path = value.str(); });
  p.add_option('\0', "help", "prints help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });

  auto res = p.run(argc, argv);
  if (res.is_error()) {
    std::cerr << "failed to parse options: " << res.error().message().str() << std::endl;
    return 2;
  }

  if (make_dump) {
    return mch::run_make_dump(trace_paths, out_path);
  }
  if (!dump_path.empty()) {
    if (ir_path.empty()) {
      std::cerr << "--replay needs -I <artifact>" << std::endl;
      return 2;
    }
    return mch::run_replay(ir_path, dump_path, twin_view_mode, fixture_lookup_paths);
  }
  std::cerr << "nothing to do: pass --make-dump or --replay (see --help)" << std::endl;
  return 2;
}
