#include "ActionPayload.h"

#include "Value.h"

#include <msgpack.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace mch {
namespace {

using Packer = msgpack::packer<msgpack::sbuffer>;

void pack_string(Packer &packer, std::string_view value) {
  packer.pack_str(static_cast<std::uint32_t>(value.size()));
  packer.pack_str_body(value.data(), static_cast<std::uint32_t>(value.size()));
}

void pack_binary(Packer &packer, const std::string &value) {
  packer.pack_bin(static_cast<std::uint32_t>(value.size()));
  packer.pack_bin_body(value.data(), static_cast<std::uint32_t>(value.size()));
}

void pack_value(Packer &packer, const Value &value, bool positional_legs = false);

bool omitted(const Value &value) {
  return value.t == VType::Null || value.t == VType::Block ||
         ((value.t == VType::Int || value.t == VType::Amount) && value.num.is_null()) ||
         (value.t == VType::Account && value.addr_none) ||
         (value.t == VType::Asset && (value.is_ton || !value.has_jetton)) ||
         (value.t == VType::Cell &&
          (value.cell.is_null() || td_boc_serialize_crc(value.cell).is_error()));
}

void pack_fields(Packer &packer, const Value::Fields &fields) {
  std::uint32_t size = 0;
  for (const auto &[key, value] : fields) {
    if (!omitted(value)) ++size;
  }
  packer.pack_map(size);
  for (const auto &[key, value] : fields) {
    if (omitted(value)) continue;
    pack_string(packer, key);
    // Positional legs need nil placeholders for empty earlier slots.
    pack_value(packer, value, key == "legs");
  }
}

void pack_value(Packer &packer, const Value &value, bool positional_legs) {
  switch (value.t) {
    case VType::Null:
    case VType::Block:
      packer.pack_nil();
      return;
    case VType::Bool:
      packer.pack(value.boolean);
      return;
    case VType::Int:
    case VType::Amount:
      if (value.num.is_null()) packer.pack_nil();
      else pack_string(packer, value.num->to_dec_string());
      return;
    case VType::Str: {
      std::string text = value.str;
      std::erase(text, '\0');
      pack_string(packer, text);
      return;
    }
    case VType::Account:
      if (value.addr_none) packer.pack_nil();
      else pack_string(packer, canonicalize_or_passthrough(value.str));
      return;
    case VType::Asset:
      if (value.is_ton || !value.has_jetton) packer.pack_nil();
      else pack_string(packer, canonicalize_or_passthrough(value.str));
      return;
    case VType::Bytes:
      pack_binary(packer, value.str);
      return;
    case VType::Cell: {
      auto boc = td_boc_serialize_crc(value.cell);
      if (boc.is_error()) packer.pack_nil();
      else pack_binary(packer, boc.ok());
      return;
    }
    case VType::List: {
      std::uint32_t size = 0;
      for (const auto &item : *value.items) {
        if (!omitted(item) || (positional_legs && item.t == VType::Null)) ++size;
      }
      packer.pack_array(size);
      for (const auto &item : *value.items) {
        if (!omitted(item) || (positional_legs && item.t == VType::Null)) pack_value(packer, item);
      }
      return;
    }
    case VType::Dict:
    case VType::Obj:
      pack_fields(packer, *value.fields);
      return;
  }
}

}  // namespace

std::string encode_action_payload(const Action &action) {
  msgpack::sbuffer buffer;
  buffer.write("\x01", 1);
  Packer packer(&buffer);
  packer.pack_map(1);
  pack_string(packer, "data");
  if ((action.data.t == VType::Dict || action.data.t == VType::Obj) && action.data.fields) {
    pack_fields(packer, *action.data.fields);
  } else {
    packer.pack_map(0);
  }
  return std::string(buffer.data(), buffer.size());
}

}  // namespace mch
