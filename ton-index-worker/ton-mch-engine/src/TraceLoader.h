// Loads an .lz4 trace fixture (LZ4-frame-compressed msgpack) produced by the
// Python indexer's recorder (tests/traces/*.lz4, schema per
// tests/utils/trace_deserializer.py). Mirrors mch-go-poc/trace/load.go: only
// the fields needed for structural matching are decoded; unknown msgpack keys
// are ignored — plus (B2) the root `interfaces` section: the per-account
// interface data Python's TestInterfaceRepository serves lookups from.
#pragma once

#include "Value.h"

#include "td/utils/Status.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mch {

struct Transaction;

struct MsgContent {
  std::string hash;
  std::string body;  // base64 BOC
};

struct Message {
  std::string msg_hash;
  std::string tx_hash;
  std::int64_t tx_lt{0};
  std::string direction;  // "in" | "out"
  std::optional<std::string> source;
  std::optional<std::string> destination;
  std::optional<std::int64_t> opcode;  // stored SIGNED (int32)
  std::optional<std::int64_t> value;
  std::optional<std::int64_t> created_lt;
  std::optional<std::int64_t> created_at;
  std::optional<bool> bounce;
  bool bounced{false};
  bool has_extra_currencies{false};  // value_extra_currencies map is non-empty
  std::optional<MsgContent> content;

  // Owning transaction, wired after decode (Python Message.transaction).
  Transaction *tx{nullptr};

  // Opcode masked to 32 bits (matches Python EventNode.get_opcode).
  std::optional<std::uint32_t> opcode32() const {
    if (!opcode) {
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(*opcode & 0xFFFFFFFF);
  }
};

struct Transaction {
  std::string hash;
  std::int64_t lt{0};
  std::int64_t now{0};
  std::int64_t mc_block_seqno{0};  // Action.mc_seqno_end (Phase E serializer)
  std::string account;
  std::string descr;  // "ord" | "tick_tock" | ...
  std::string orig_status;
  std::string end_status;
  std::optional<std::string> skipped_reason;  // absent from fixtures (always null in tests)
  std::optional<std::int64_t> compute_exit_code;
  bool aborted{false};
  std::vector<std::unique_ptr<Message>> messages;
};

struct Trace {
  std::string trace_id;
  // TRACE-LEVEL aggregates — the five create_unknown_action reads off the Trace
  // ROW in Python (block_tree_serializer.py:1692-1703).
  //
  // EVERY loader must set them: this fixture loader from the `trace` header,
  // SchemaTraceLoader (the PRODUCTION path, used by both classifier actors) via
  // fill_trace_aggregates. Leaving them at zero on any path is a landmine for
  // every future Trace consumer — exactly what W4's unknown row walked into,
  // and why the derivation lives in the loader rather than at the one call site
  // that happened to notice.
  //
  // The derived form is min/max over `transactions`, and that is the RIGHT
  // definition on every path, not a fallback:
  //  - fixtures: identical to the trace header on 165/165 corpus traces, and
  //    the --actions (header-sourced) vs --actions-schema/--actions-prod
  //    (derived) gates re-prove it on every run;
  //  - emulated: there is nothing else to defer to. The emulator's ::Trace
  //    (TraceEmulator.h:127-157) carries NO trace-level lt/utime/seqno at all,
  //    and Python does not either — it derives them the same way, per emission
  //    (event_deserializer.py:146-151: start from the ROOT tx, end from
  //    max(lt)/max(now)). So a pending trace's end_lt IS max(tx.lt) on both
  //    sides; there is no projected end to miss.
  // Two knowingly-accepted differences from Python on the emulated path:
  // start_lt here is min(lt) where Python takes the root tx's lt (identical for
  // any well-formed trace, since the root is the earliest), and mc_seqno_end is
  // derived here where Python leaves it None (harmless — ton-index-go has no
  // field for it, em2-writeback-design.md §2.3).
  std::int64_t start_lt{0}, end_lt{0}, start_utime{0}, end_utime{0}, mc_seqno_end{0};
  std::vector<std::unique_ptr<Transaction>> transactions;
  // Root `interfaces` section, decoded generically: account (raw uppercase
  // string, the msgpack key) -> Dict{InterfaceName -> Dict{field -> value}}.
  // Kept raw enough that a LookupSource can apply per-kind semantics on top
  // (B2). Msgpack scalars map: str->Str, int->Int, bool->Bool, nil->Null,
  // bin->Bytes, map->Dict, array->List; floats have no Value kind -> Null.
  std::map<std::string, Value> interfaces;
};

// Reads, decompresses and decodes one fixture file.
td::Result<Trace> load_trace(const std::string &path);

// Derives Trace's five aggregates from `trace.transactions`: min/max lt, min/max
// now, max mc_block_seqno. For loaders with no trace header — i.e. the
// production SchemaTraceLoader.
//
// This reproduces the fixture header's values rather than defining new ones:
// across all 165 corpus fixtures the header's five ARE exactly these min/max
// (verified 0 disagreements). The equivalence is not taken on faith — the
// `--actions` vs `--actions-schema`/`--actions-prod` gates compare a
// header-sourced dump against a derived one and must stay byte-identical.
void fill_trace_aggregates(Trace &trace);

}  // namespace mch
