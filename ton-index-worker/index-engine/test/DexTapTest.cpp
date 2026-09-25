#include "DexTap.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "vm/cells/CellBuilder.h"

namespace {

int failures = 0;

void check(bool ok, const char *description) {
  if (!ok) {
    std::cerr << "FAIL: " << description << '\n';
    ++failures;
  }
}

td::Bits256 bits_from_hex(const std::string &hex) {
  td::Bits256 bits;
  CHECK(bits.from_hex(hex) == 256);
  return bits;
}

td::Ref<vm::Cell> small_cell(int value) {
  vm::CellBuilder builder;
  builder.store_long(value, 32);
  return builder.finalize();
}

schema::AccountState pool_state(const block::StdAddress &address, std::uint64_t lt, const std::string &code_hash) {
  schema::AccountState state;
  state.account = address;
  state.timestamp = 1790000000;
  state.balance.grams = td::make_refint(1234567);
  state.code = small_cell(1);
  state.data = small_cell(static_cast<int>(lt));
  state.code_hash = bits_from_hex(code_hash);
  state.data_hash = state.data->get_hash().bits();
  state.last_trans_lt = lt;
  return state;
}

schema::Transaction transaction(const block::StdAddress &address) {
  schema::Transaction tx;
  tx.account = address;
  return tx;
}

int bind_listener(const std::string &path) {
  int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  ::unlink(path.c_str());
  CHECK(::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
  return fd;
}

std::string receive(int fd) {
  char buffer[1 << 16];
  auto size = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
  return size > 0 ? std::string(buffer, static_cast<std::size_t>(size)) : std::string();
}

const std::string kStonfiV1 = "82566AD72B6568FE7276437D3B0C911AAB65ED701C13601941B2917305E81C11";

void test_line_format() {
  DexTapRecord record{1, 2, 3, 4, "0:AB", 5, "6", "C0", "D0", "Y29kZQ==", "ZGF0YQ==", 7};
  check(dex_tap_line(record) ==
            "{\"v\":1,\"session\":1,\"seq\":2,\"mc_seqno\":3,\"utime\":4,\"account\":\"0:AB\",\"last_trans_lt\":5,"
            "\"balance\":\"6\",\"code_hash\":\"C0\",\"data_hash\":\"D0\",\"code\":\"Y29kZQ==\",\"data\":\"ZGF0YQ==\","
            "\"tx_count\":7}",
        "line format");
}

void test_whitelist() {
  check(dex_tap_code_hash_whitelisted(bits_from_hex(kStonfiV1)), "STON.fi v1 pool code is whitelisted");
  check(!dex_tap_code_hash_whitelisted(td::Bits256::zero()), "unknown code is not whitelisted");
}

void test_publish() {
  std::string path = "/tmp/dex-tap-test-" + std::to_string(::getpid()) + ".sock";
  int listener = bind_listener(path);

  auto tap_result = DexTap::open(path);
  check(tap_result.is_ok(), "open tap");
  auto tap = tap_result.move_as_ok();

  block::StdAddress pool(0, bits_from_hex(std::string(64, 'A')));
  block::StdAddress other(0, bits_from_hex(std::string(64, 'B')));

  ParsedBlock block;
  block.account_states_.push_back(pool_state(pool, 100, kStonfiV1));
  block.account_states_.push_back(pool_state(pool, 200, kStonfiV1));
  block.account_states_.push_back(pool_state(other, 300, std::string(64, '0')));
  schema::Block shard_block;
  shard_block.transactions = {transaction(pool), transaction(pool), transaction(other)};
  block.blocks_.push_back(shard_block);

  tap->publish(42, block);
  auto line = receive(listener);
  check(line.find("\"mc_seqno\":42") != std::string::npos, "mc_seqno is sent");
  check(line.find("\"account\":\"0:" + std::string(64, 'A') + "\"") != std::string::npos, "pool address is sent");
  check(line.find("\"last_trans_lt\":200") != std::string::npos, "latest state within the block wins");
  check(line.find("\"tx_count\":2") != std::string::npos, "pool transactions are counted");
  check(line.find("\"balance\":\"1234567\"") != std::string::npos, "balance is sent");
  check(receive(listener).empty(), "non-pool accounts are skipped");
  check(tap->stats().sent == 1 && tap->stats().dropped == 0, "one datagram sent");

  ::close(listener);
  ::unlink(path.c_str());
  tap->publish(43, block);
  check(tap->stats().dropped == 1, "datagram dropped without a listener");
}

}  // namespace

int main() {
  test_line_format();
  test_whitelist();
  test_publish();
  if (failures) {
    std::cerr << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "dex tap tests passed\n";
  return 0;
}
