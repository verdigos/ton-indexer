#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "td/utils/Status.h"

#include "IndexData.h"

// Prototype bridge for DEX price research (docs/dex-prices/mvp-plan.md, stage M-1).
//
// For every masterchain block, sends one JSON line per DEX pool whose state changed in the
// block to a local unix datagram socket. The socket is non-blocking and lossy by design: when
// no listener is bound or its buffer is full, the datagram is dropped and counted, so indexing
// never waits on the consumer. Each datagram carries a per-process sequence number, so the
// listener can count losses.
class DexTap {
public:
  struct Stats {
    std::uint64_t sent{0};
    std::uint64_t dropped{0};
    std::uint64_t serialize_errors{0};
  };

  static td::Result<std::unique_ptr<DexTap>> open(const std::string& socket_path);
  ~DexTap();

  DexTap(const DexTap&) = delete;
  DexTap& operator=(const DexTap&) = delete;

  void publish(std::uint32_t mc_seqno, const ParsedBlock& block);
  const Stats& stats() const { return stats_; }

private:
  DexTap(int fd, std::string socket_path, std::uint64_t session);
  void send_line(const std::string& line);

  int fd_;
  std::string socket_path_;
  std::uint64_t session_;
  std::uint64_t seq_{0};
  Stats stats_;
};

bool dex_tap_code_hash_whitelisted(const td::Bits256& code_hash);

struct DexTapRecord {
  std::uint64_t session;
  std::uint64_t seq;
  std::uint32_t mc_seqno;
  std::uint32_t utime;
  std::string account;       // raw form, "0:HEX"
  std::uint64_t last_trans_lt;
  std::string balance;       // nanotons, decimal
  std::string code_hash;     // hex
  std::string data_hash;     // hex
  std::string code_boc;      // base64
  std::string data_boc;      // base64
  std::uint32_t tx_count;
};

// One JSON object without a trailing newline. Every string field is hex, base64 or a decimal
// number, so nothing needs escaping.
std::string dex_tap_line(const DexTapRecord& record);
