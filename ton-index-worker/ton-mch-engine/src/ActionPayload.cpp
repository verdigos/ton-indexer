#include "ActionPayload.h"
#include "Value.h"
#include "payload_keys_generated.h"
#include "td/utils/base64.h"
#include <msgpack.hpp>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mch {
namespace {
using Packer = msgpack::packer<msgpack::sbuffer>;

void pack_string(Packer &pk, std::string_view s) {
  pk.pack_str(static_cast<std::uint32_t>(s.size()));
  pk.pack_str_body(s.data(), static_cast<std::uint32_t>(s.size()));
}
void pack_binary(Packer &pk, std::string_view s) {
  pk.pack_bin(static_cast<std::uint32_t>(s.size()));
  pk.pack_bin_body(s.data(), static_cast<std::uint32_t>(s.size()));
}
std::string value_text(const Value &v) {
  if (v.t == VType::Int || v.t == VType::Amount) return v.num->to_dec_string();
  if (v.t == VType::Account || v.t == VType::Asset) return canonicalize_or_passthrough(v.str);
  return v.str;
}
[[noreturn]] void invalid(std::string_view name, std::string_view expected, std::string_view text) {
  std::string sample(text.substr(0, 48));
  std::replace_if(sample.begin(), sample.end(), [](char c) { return c == '\r' || c == '\n'; }, ' ');
  throw std::runtime_error("payload key " + std::string(name) + " expects " + std::string(expected) + ": " + sample);
}
std::string typed_text(const Value &v, std::string_view name, std::string_view expected) {
  if (v.t != VType::Str && v.t != VType::Int && v.t != VType::Amount)
    invalid(name, expected, "non-text value");
  return value_text(v);
}
std::string typed_str(const Value &v, std::string_view name, std::string_view expected) {
  if (v.t == VType::Str) return v.str;
  invalid(name, expected, v.t == VType::Int || v.t == VType::Amount ? value_text(v) : "non-text value");
}
std::string cell_bytes(const Value &v, std::string_view name) {
  auto boc = td_boc_serialize_crc(v.cell);
  if (boc.is_error()) invalid(name, "serializable boc", boc.error().message().str());
  return boc.move_as_ok();
}
const PayloadKeyInfo &key_info(std::string_view name) {
  auto it = std::lower_bound(payload_keys.begin(), payload_keys.end(), name,
                             [](const PayloadKeyInfo &row, std::string_view n) { return row.name < n; });
  if (it == payload_keys.end() || it->name != name) invalid(name, "registered key", name);
  return *it;
}
int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
std::string hex_bytes(std::string_view text, bool uppercase) {
  if (text.size() % 2) return {};
  std::string bytes;
  bytes.reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2) {
    if (uppercase ? ((text[i] >= 'a' && text[i] <= 'f') || (text[i + 1] >= 'a' && text[i + 1] <= 'f'))
                  : ((text[i] >= 'A' && text[i] <= 'F') || (text[i + 1] >= 'A' && text[i + 1] <= 'F')))
      return {};
    int a = hex_digit(text[i]), b = hex_digit(text[i + 1]);
    if (a < 0 || b < 0) return {};
    bytes.push_back(static_cast<char>((a << 4) | b));
  }
  return bytes;
}
std::string base64_bytes(std::string_view name, std::string_view text, bool hash) {
  auto decoded = td::base64_decode(td::Slice(text.data(), text.size()));
  if (decoded.is_error() || td::base64_encode(td::Slice(decoded.ok())) != text ||
      (hash && decoded.ok().size() != 32))
    invalid(name, hash ? "32-byte padded base64 hash" : "padded base64 boc", text);
  return decoded.move_as_ok();
}
void pack_fields(Packer &pk, const Value::Fields &fields);
void pack_plain(Packer &pk, const Value &v, std::string_view name) {
  switch (v.t) {
    case VType::Null: pk.pack_nil(); return;
    case VType::Str: pack_string(pk, typed_text(v, name, "string")); return;
    case VType::List: {
      pk.pack_array(static_cast<std::uint32_t>(v.items->size()));
      for (const auto &item : *v.items) pack_plain(pk, item, name);
      return;
    }
    case VType::Dict: case VType::Obj: pack_fields(pk, *v.fields); return;
    default: invalid(name, "container", "non-container value");
  }
}
void pack_typed(Packer &pk, const Value &v, const PayloadKeyInfo &key) {
  const auto name = key.name;
  switch (key.type) {
    case PayloadKeyType::Other: pack_plain(pk, v, name); return;
    case PayloadKeyType::String:
      pack_string(pk, typed_text(v, name, "string")); return;
    case PayloadKeyType::Bool:
      if (v.t != VType::Bool) invalid(name, "bool", "non-bool value");
      pk.pack(v.boolean); return;
    case PayloadKeyType::Address: {
      auto text = v.t == VType::Account || v.t == VType::Asset
                      ? canonicalize_or_passthrough(v.str)
                      : typed_str(v, name, "W:HEX address");
      auto colon = text.find(':');
      if (colon == std::string::npos || text.size() != colon + 65) invalid(name, "W:HEX address", text);
      int wc = 0;
      auto [ptr, ec] = std::from_chars(text.data(), text.data() + colon, wc);
      if (ec != std::errc{} || ptr != text.data() + colon ||
          std::to_string(wc) != text.substr(0, colon) || wc < -128 || wc > 127)
        invalid(name, "W:HEX address", text);
      auto hash = hex_bytes(std::string_view(text).substr(colon + 1), true);
      if (hash.size() != 32) invalid(name, "W:HEX address", text);
      std::string bytes(1, static_cast<char>(wc));
      bytes += hash;
      pack_binary(pk, bytes); return;
    }
    case PayloadKeyType::Boc:
      if (v.t == VType::Bytes) { pack_binary(pk, v.str); return; }
      if (v.t == VType::Cell) { pack_binary(pk, cell_bytes(v, name)); return; }
      pack_binary(pk, base64_bytes(name, typed_str(v, name, "padded base64 boc"), false)); return;
    case PayloadKeyType::Hash: case PayloadKeyType::HashHex: case PayloadKeyType::Hash0x: {
      auto text = typed_str(v, name, "32-byte hash");
      if (key.type == PayloadKeyType::Hash) { pack_binary(pk, base64_bytes(name, text, true)); return; }
      bool prefixed = key.type == PayloadKeyType::Hash0x;
      if (prefixed && !text.starts_with("0x")) invalid(name, "0x lowercase 32-byte hash", text);
      auto bytes = hex_bytes(std::string_view(text).substr(prefixed ? 2 : 0), false);
      if (bytes.size() != 32) invalid(name, prefixed ? "0x lowercase 32-byte hash" : "lowercase 32-byte hash", text);
      pack_binary(pk, bytes); return;
    }
    case PayloadKeyType::Uint: {
      auto text = typed_text(v, name, "canonical unsigned decimal");
      if (text.empty() || (text.size() > 1 && text.front() == '0') ||
          !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
        invalid(name, "canonical unsigned decimal", text);
      std::uint64_t small = 0;
      auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), small);
      if (ec == std::errc{} && ptr == text.data() + text.size()) { pk.pack(small); return; }
      std::string bytes(1, '\0');
      for (char digit : text) {
        unsigned carry = static_cast<unsigned>(digit - '0');
        for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
          carry += static_cast<unsigned char>(*it) * 10;
          *it = static_cast<char>(carry & 255);
          carry >>= 8;
        }
        while (carry) { bytes.insert(bytes.begin(), static_cast<char>(carry & 255)); carry >>= 8; }
      }
      while (bytes.size() > 1 && bytes.front() == '\0') bytes.erase(bytes.begin());
      pack_binary(pk, bytes); return;
    }
  }
}
void pack_fields(Packer &pk, const Value::Fields &fields) {
  std::vector<std::pair<const PayloadKeyInfo *, const Value *>> ordered;
  ordered.reserve(fields.size());
  for (const auto &[name, value] : fields) {
    const auto &key = key_info(name);
    if (!omitted(value)) ordered.emplace_back(&key, &value);
  }
  std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) { return a.first->id < b.first->id; });
  for (std::size_t i = 1; i < ordered.size(); ++i)
    if (ordered[i - 1].first->id == ordered[i].first->id) invalid(ordered[i].first->name, "unique key", "duplicate");
  pk.pack_map(static_cast<std::uint32_t>(ordered.size()));
  for (const auto &[key, value] : ordered) { pk.pack(key->id); pack_typed(pk, *value, *key); }
}
}  // namespace

std::string encode_action_payload(const Action &action) {
  msgpack::sbuffer buffer;
  buffer.write("\x01", 1);
  Packer pk(&buffer);
  if (action.data.t == VType::Dict && action.data.fields)
    pack_fields(pk, *action.data.fields);
  else pk.pack_map(0);
  return std::string(buffer.data(), buffer.size());
}
std::string action_payload_bytes(const Action &action) {
  return action.encoded_payload.empty() ? encode_action_payload(action) : action.encoded_payload;
}
}  // namespace mch
