#pragma once

#include "IndexData.h"
#include "payload_keys_generated.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mch {

inline schema::Transaction schema_tx(std::uint8_t tag) {
  schema::Transaction tx{};
  tx.hash.set_zero();
  tx.hash.data()[31] = tag;
  tx.account = block::StdAddress(0, tx.hash);
  tx.lt = 100;
  tx.now = 200;
  tx.mc_seqno = 300;
  tx.orig_status = schema::AccountStatus::active;
  tx.end_status = schema::AccountStatus::active;
  tx.description = schema::TransactionDescr_ord{};
  schema::Message message{};
  message.hash.set_zero();
  message.hash.data()[31] = tag + 1;
  message.source = "0:" + std::string(64, 'B');
  message.destination = "0:" + tx.hash.to_hex();
  message.opcode = 0x11223344;
  message.value = schema::CurrencyCollection{td::make_refint(1000), {}};
  tx.in_msg = std::move(message);
  return tx;
}

inline std::uint8_t id_of(std::string_view name) {
  for (const auto &key : payload_keys) {
    if (key.name == name) return key.id;
  }
  throw std::out_of_range(std::string(name));
}

}  // namespace mch
