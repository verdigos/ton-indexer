#include "DexTap.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "td/utils/base64.h"
#include "td/utils/logging.h"
#include "vm/boc.h"

#include "convert-utils.h"

namespace {

// Pool code hashes of the DEXes in scope, collected from live pools on 2026-09-25
// (docs/dex-prices/mvp-plan.md, §4.1.1). The DeDust ones match smc-interfaces/Tokens.cpp.
const std::unordered_set<std::string> kPoolCodeHashes{
    // STON.fi v1
    "82566AD72B6568FE7276437D3B0C911AAB65ED701C13601941B2917305E81C11",
    // STON.fi v2: constant_product (two versions), stableswap, weighted_const_product, weighted_stableswap
    "EC614EA4AAEA3F7768606F1C1632B3374D3DE096A1E7C4BA43C8009C487FEE9D",
    "CF5D0B99FA704E7CF2C9D50A8FF8B8BC7CE0B8A74E414B9C279AC544E7AADE05",
    "FBC7E8FCCA72C2B9C078B359FFA936F46384491B895B6577B0A6CB3F569040BC",
    "F04A14C3231221056C3499965E4604417E324F8E9121D840120D803288715594",
    "DAC47636AE899081EBD4F47DC90EF9DE98456B1000591069773F683C6D601FA9",
    // DeDust v1
    "1275095B6DA3911292406F4F4386F9E780099B854C6DEE9EE2895DDCE70927C1",
    "778F0D3FE6482C50888970DF5E787F40F3A4AB282170C035A5920877058C99D3",
    "C0F9D14FBC8E14F0D72CBA2214165EEE35836AB174130912BAF9DBFA43EAD562",
    // DeDust v2: CPMM v2.0, v2.1
    "3997A5C1EE8923E93CF3A6A98EA3EF9482C64DD8AEA2BA8365689E14D99C760D",
    "5851780D2386E989B9AB956DA7CEC5C69A667B9E525F628DCBA5EB02C1EE0367",
};

td::Result<std::string> boc_base64(const td::Ref<vm::Cell>& cell) {
  TRY_RESULT(boc, vm::std_boc_serialize(cell, 2));
  return td::base64_encode(boc.as_slice());
}

}  // namespace

bool dex_tap_code_hash_whitelisted(const td::Bits256& code_hash) {
  return kPoolCodeHashes.count(code_hash.to_hex()) > 0;
}

std::string dex_tap_line(const DexTapRecord& r) {
  std::ostringstream out;
  out << "{\"v\":1"
      << ",\"session\":" << r.session
      << ",\"seq\":" << r.seq
      << ",\"mc_seqno\":" << r.mc_seqno
      << ",\"utime\":" << r.utime
      << ",\"account\":\"" << r.account << "\""
      << ",\"last_trans_lt\":" << r.last_trans_lt
      << ",\"balance\":\"" << r.balance << "\""
      << ",\"code_hash\":\"" << r.code_hash << "\""
      << ",\"data_hash\":\"" << r.data_hash << "\""
      << ",\"code\":\"" << r.code_boc << "\""
      << ",\"data\":\"" << r.data_boc << "\""
      << ",\"tx_count\":" << r.tx_count
      << "}";
  return out.str();
}

td::Result<std::unique_ptr<DexTap>> DexTap::open(const std::string& socket_path) {
  sockaddr_un addr{};
  if (socket_path.empty() || socket_path.size() >= sizeof(addr.sun_path)) {
    return td::Status::Error("dex tap socket path is empty or too long");
  }
  int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return td::Status::PosixError(errno, "dex tap socket()");
  }
  // A larger send buffer absorbs bursts of pool updates within one masterchain block.
  int sndbuf = 4 << 20;
  ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  auto session = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  return std::unique_ptr<DexTap>(new DexTap(fd, socket_path, session));
}

DexTap::DexTap(int fd, std::string socket_path, std::uint64_t session)
    : fd_(fd), socket_path_(std::move(socket_path)), session_(session) {
  LOG(INFO) << "DEX tap enabled: " << socket_path_ << " session=" << session_;
}

DexTap::~DexTap() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

void DexTap::send_line(const std::string& line) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, socket_path_.data(), socket_path_.size());
  auto sent = ::sendto(fd_, line.data(), line.size(), MSG_DONTWAIT, reinterpret_cast<const sockaddr*>(&addr),
                       sizeof(addr));
  if (sent == static_cast<ssize_t>(line.size())) {
    ++stats_.sent;
    return;
  }
  // No listener (ENOENT, ECONNREFUSED), a full buffer (EAGAIN) or an oversized datagram (EMSGSIZE):
  // drop it and keep indexing.
  ++stats_.dropped;
  if (stats_.dropped == 1 || stats_.dropped % 10000 == 0) {
    LOG(WARNING) << "DEX tap dropped " << stats_.dropped << " datagrams, last error: " << std::strerror(errno);
  }
}

void DexTap::publish(std::uint32_t mc_seqno, const ParsedBlock& block) {
  // The last state of each pool within the masterchain block, as BlockInterfaceProcessor picks it.
  std::unordered_map<block::StdAddress, const schema::AccountState*> latest;
  for (const auto& state : block.account_states_) {
    if (!state.code_hash || !state.data_hash || state.code.is_null() || state.data.is_null()) {
      continue;
    }
    if (!dex_tap_code_hash_whitelisted(state.code_hash.value())) {
      continue;
    }
    auto it = latest.find(state.account);
    if (it == latest.end() || state.last_trans_lt > it->second->last_trans_lt) {
      latest[state.account] = &state;
    }
  }
  if (latest.empty()) {
    return;
  }

  std::unordered_map<block::StdAddress, std::uint32_t> tx_counts;
  for (const auto& blk : block.blocks_) {
    for (const auto& tx : blk.transactions) {
      if (latest.count(tx.account)) {
        ++tx_counts[tx.account];
      }
    }
  }

  for (const auto& [address, state] : latest) {
    auto code = boc_base64(state->code);
    auto data = boc_base64(state->data);
    if (code.is_error() || data.is_error()) {
      ++stats_.serialize_errors;
      continue;
    }
    DexTapRecord record{
        .session = session_,
        .seq = ++seq_,
        .mc_seqno = mc_seqno,
        .utime = state->timestamp,
        .account = convert::to_raw_address(address),
        .last_trans_lt = state->last_trans_lt,
        .balance = state->balance.grams.is_null() ? "0" : state->balance.grams->to_dec_string(),
        .code_hash = state->code_hash->to_hex(),
        .data_hash = state->data_hash->to_hex(),
        .code_boc = code.move_as_ok(),
        .data_boc = data.move_as_ok(),
        .tx_count = tx_counts[address],
    };
    send_line(dex_tap_line(record));
  }
}
