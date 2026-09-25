#pragma once

#include "IndexData.h"
#include "payload_keys_generated.h"

#include <cstdint>
#include <cstring>
#include <msgpack.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mch {

inline const msgpack::object *at(const msgpack::object &map, const char *key) {
  if (map.type != msgpack::type::MAP) return nullptr;
  const auto n = std::strlen(key);
  for (std::uint32_t i = 0; i < map.via.map.size; ++i) {
    const auto &candidate = map.via.map.ptr[i].key;
    if (candidate.type == msgpack::type::STR && candidate.via.str.size == n &&
        std::memcmp(candidate.via.str.ptr, key, n) == 0) {
      return &map.via.map.ptr[i].val;
    }
  }
  return nullptr;
}

inline const msgpack::object *at_id(const msgpack::object &map, std::uint8_t id) {
  if (map.type != msgpack::type::MAP) return nullptr;
  for (std::uint32_t i = 0; i < map.via.map.size; ++i) {
    const auto &entry = map.via.map.ptr[i];
    if (entry.key.type == msgpack::type::POSITIVE_INTEGER && entry.key.via.u64 == id) {
      return &entry.val;
    }
  }
  return nullptr;
}

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
