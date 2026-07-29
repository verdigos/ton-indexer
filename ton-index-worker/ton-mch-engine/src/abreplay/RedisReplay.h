// EM2 W7(iii) — the Redis->classify replay adapter's data seam
// (em2-writeback-design.md §8 phase 1; twins/README.md "the dump format the prod
// sweep must produce").
//
// A phase-1 sweep gives us one `HGETALL <trace_key>` per pending trace. This
// header is the boundary between those RAW BYTES and the inputs the existing
// classify path takes (schema::Transaction vector + ParsedBlockLookupSource::
// InterfaceMap). Everything emulator-shaped — Serializer.hpp, TraceEmulator.h,
// msgpack — stays inside RedisReplay.cpp, which is the ONLY TU that includes
// them; nothing under src/emu/ or ton-trace-emulator/ is touched.
//
// What replay CANNOT recover, both inherent to the wire format and both
// documented upstream (twins/README.md, emu-integration-plan.md §2.2):
//   - message `value` is a bare uint64 grams, so extra currencies are lost;
//   - RedisTraceNode's descr is ord-only, so tick-tock transactions do not
//     round-trip.
// Neither affects the action diff phase 1 measures.
#pragma once

#include "ParsedBlockLookupSource.h"  // InterfaceMap
#include "TraceLoader.h"              // mch::Trace (the --make-dump input)

#include "IndexData.h"  // schema::Transaction (tondb-scanner, PUBLIC)

#include "td/utils/Status.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mch {

// One trace's Redis hash, exactly as a read-only HGETALL sweep produces it.
// Values are the RAW binary field values (the JSONL carries them base64'd).
struct RedisTraceDump {
  std::string trace_key;                          // base64(ext_in_msg_hash_norm)
  std::string root_node;                          // the `root_node` field, verbatim
  std::map<std::string, std::string> nodes;       // node key -> msgpack(RedisTraceNode)
  std::map<std::string, std::string> interfaces;  // "wc:HEX" -> msgpack(AddressInterfaces)
  // DRY-RUN ONLY, and absent from a prod sweep. In Redis the trace key IS the
  // trace id, so `--replay` uses trace_key for both. The corpus round-trip is
  // the one case where they differ: the join key has to be the fixture FILENAME
  // (that is how mk_hset_dump.py keys Python's side) while the classifier must
  // still see the fixture's own trace_id, because create_unknown_action derives
  // the `unknown` row's action_id from it.
  std::string trace_id;
};

// The classify path's inputs, recovered from one dump.
struct RedisReplayTrace {
  // Root-first breadth-first over the node graph, mirroring the order
  // TraceInserter itself flattens a trace in (TraceInserter.cpp:71-108). to_tree
  // re-sorts by lt anyway, so this order is only a tie-break.
  std::vector<schema::Transaction> txs;
  // Per-tx emission finality, index-parallel to `txs`. serialize_actions takes
  // the min of these as the `finality` wire key (em2-writeback-design.md §2.3).
  std::vector<std::uint8_t> node_finality;
  ParsedBlockLookupSource::InterfaceMap ifaces;

  // Fidelity accounting — every one of these is a silent-loss candidate, so the
  // replay driver prints them rather than letting them pass unnoticed.
  std::size_t unreachable_nodes{0};  // node blobs not reachable from root_node
  std::size_t nodes_undecodable{0};
  std::size_t iface_blobs_undecodable{0};
  std::size_t iface_variants_dropped{0};  // decoded, but no tier-1 kind maps to them
};

// Decode + convert. Fails only when the trace cannot be assembled at all (no
// root_node, root blob missing/undecodable); per-node and per-interface losses
// are counted, not fatal.
td::Result<RedisReplayTrace> redis_dump_to_replay(const RedisTraceDump &dump);

// --make-dump (HARNESS ONLY): the reverse, so the corpus can manufacture a dump
// in the prod sweep's exact shape. Packs with the EMULATOR'S OWN structs, which
// is what makes the round-trip a test of the reader rather than of a private
// copy of the format.
//
// `trace_key` is the caller's join key (the fixture filename in the dry run —
// mk_hset_dump.py keys Python's side the same way).
td::Result<RedisTraceDump> fixture_to_redis_dump(const std::string &trace_key, const Trace &trace);

}  // namespace mch
