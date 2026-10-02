// Telegram wallet key rotation parsers. Keys and signatures render as
// lowercase hex strings.
#include "parse/Parsers.h"

#include "parse/PSlice.h"

#include "td/utils/misc.h"
#include "vm/cellslice.h"

#include <optional>
#include <string>
#include <utility>

namespace mch {

namespace {

constexpr unsigned long long kChangePublicKeyInternal = 0xFBBA99C7;
constexpr unsigned long long kChangePublicKeyExternal = 0xFBBA99C8;
constexpr unsigned long long kKeyChangedLog = 0xEBA19948;

std::string fetch_hex(vm::CellSlice &cs, unsigned bits) {
  std::string raw(bits / 8, '\0');
  cs.fetch_bytes(reinterpret_cast<unsigned char *>(raw.data()), bits / 8);
  return td::hex_encode(raw);
}

// A ref holding exactly `bits` data bits and no refs; any other shape is null.
std::optional<std::string> bits_ref_hex(const td::Ref<vm::Cell> &cell, unsigned bits) {
  auto r_cs = open_ref_cell(cell);
  if (r_cs.is_error()) {
    return std::nullopt;
  }
  auto cs = r_cs.move_as_ok();
  if (cs.size() != bits || cs.size_refs() != 0) {
    return std::nullopt;
  }
  return fetch_hex(cs, bits);
}

}  // namespace

// ChangePublicKeyRequestI/E: signature:bits512 opcode:uint32 subwallet_id:uint32
// valid_until:uint32 seqno:uint32 new_public_key:bits256, then the rotation
// signature (512 bits) and encrypted old private key (256 bits) as refs.
td::Result<Value> parse_tg_wallet_change_public_key_request(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(512 + 32 + 96 + 256)) {
    return td::Status::Error("tg wallet change key: underflow");
  }
  cs.advance(512);
  auto opcode = cs.fetch_ulong(32);
  if (opcode != kChangePublicKeyInternal && opcode != kChangePublicKeyExternal) {
    return td::Status::Error("tg wallet change key: unexpected opcode");
  }
  cs.advance(96);  // subwallet_id, valid_until, seqno
  std::string new_public_key = fetch_hex(cs, 256);
  std::optional<std::string> rotation_signature;
  if (cs.size_refs() != 0) {
    rotation_signature = bits_ref_hex(cs.fetch_ref(), 512);
  }
  Value::Fields ns;
  ns.emplace_back("new_public_key", Value::make_str(std::move(new_public_key)));
  ns.emplace_back("rotation_signature",
                  rotation_signature ? Value::make_str(std::move(*rotation_signature)) : Value::null());
  return Value::make_dict(std::move(ns));
}

// The wallet's rotation log: opcode:uint32 encrypted_old_private_key:bits256.
td::Result<Value> parse_tg_wallet_key_changed_log(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (cs.size() != 32 + 256 || cs.fetch_ulong(32) != kKeyChangedLog) {
    return td::Status::Error("tg wallet key changed log: unexpected shape");
  }
  Value::Fields ns;
  ns.emplace_back("encrypted_old_private_key", Value::make_str(fetch_hex(cs, 256)));
  return Value::make_dict(std::move(ns));
}

}  // namespace mch
