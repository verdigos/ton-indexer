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

}  // namespace mch
