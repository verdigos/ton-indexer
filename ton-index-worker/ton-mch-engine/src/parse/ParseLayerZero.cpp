// LayerZero message parser (messages/layerzero.py:149-208, 1377-1383). See
// parse/PSlice.h for shared machinery and MsgParse.cpp's header for the
// pytoniq-parity catalogue.
//
// LayerZeroOappExecuteCallback: cell = slice.to_cell(); packet =
// LayerZeroPacket(cell.refs[0].refs[0]). The twin dumps vars() recursively, so
// the Obj mirrors EVERY Python attribute, incl. b (the source cell), s (the
// slice AFTER all loads -> remainder rebuilt as a cell) and the asserted
// header ints. guid/src_oapp/dst_oapp are Python hex(): "0x" + lowercase
// MINIMAL digits (leading zeros stripped — unlike the 64-digit account form).
// message is '0x' + bits.tobytes().hex(): bit-level bytes, right-padded to a
// byte boundary, leading zeros KEPT.
#include "parse/Parsers.h"

#include "parse/PSlice.h"

#include "common/refint.h"
#include "td/utils/base64.h"
#include "vm/boc.h"
#include "vm/cellslice.h"
#include "vm/cells/CellBuilder.h"

#include <string>
#include <utility>

namespace mch {

namespace {

td::Result<std::string> lz_hex_min(vm::CellSlice &cs, int bits) {
  auto v = cs.fetch_int256(bits, false);
  if (v.is_null()) {
    return td::Status::Error("lz: int underflow");
  }
  return "0x" + td::hex_string(v);
}

td::Result<Value> lz_parse_path(const td::Ref<vm::Cell> &c) {
  PSlice ps = pslice_from_cell(c);
  auto hi = ps.cs.fetch_int256(152, false);
  if (hi.is_null() || td::dec_string(hi) != "8903714975572488637007080065659") {
    return td::Status::Error("lz path: header info mismatch");
  }
  auto hf = ps.cs.fetch_int256(198, true);
  if (hf.is_null() || td::dec_string(hf) != "-1") {
    return td::Status::Error("lz path: header filler mismatch");
  }
  if (!ps.cs.have(32)) {
    return td::Status::Error("lz path: src_eid underflow");
  }
  auto src_eid = ps.cs.fetch_ulong(32);
  TRY_RESULT(src_oapp, lz_hex_min(ps.cs, 256));
  if (!ps.cs.have(32)) {
    return td::Status::Error("lz path: dst_eid underflow");
  }
  auto dst_eid = ps.cs.fetch_ulong(32);
  TRY_RESULT(dst_oapp, lz_hex_min(ps.cs, 256));
  TRY_RESULT(rem, pslice_to_cell(ps));
  Value::Fields f;
  f.emplace_back("b", Value::make_cell(c));
  f.emplace_back("s", Value::make_cell(std::move(rem)));
  f.emplace_back("header_info", Value::make_int(std::move(hi)));
  f.emplace_back("header_filler", Value::make_int(std::move(hf)));
  f.emplace_back("src_eid", Value::make_int64(static_cast<std::int64_t>(src_eid)));
  f.emplace_back("src_oapp", Value::make_str(std::move(src_oapp)));
  f.emplace_back("dst_eid", Value::make_int64(static_cast<std::int64_t>(dst_eid)));
  f.emplace_back("dst_oapp", Value::make_str(std::move(dst_oapp)));
  return Value::make_obj(std::move(f));
}

td::Result<Value> lz_parse_packet(const td::Ref<vm::Cell> &c) {
  PSlice ps = pslice_from_cell(c);
  auto hi = ps.cs.fetch_int256(152, false);
  if (hi.is_null() || td::dec_string(hi) != "417359019239977417716476838698419835") {
    return td::Status::Error("lz packet: header info mismatch");
  }
  auto hf = ps.cs.fetch_int256(198, true);
  if (hf.is_null() || td::dec_string(hf) != "-1") {
    return td::Status::Error("lz packet: header filler mismatch");
  }
  if (ps.off >= ps.refs.size()) {
    return td::Status::Error("lz packet: path ref missing");
  }
  auto path_cell = ps.refs[ps.off++];
  TRY_RESULT(path, lz_parse_path(path_cell));
  if (ps.off >= ps.refs.size()) {
    return td::Status::Error("lz packet: message ref missing");
  }
  auto msg_cell = ps.refs[ps.off++];
  std::string message;
  {
    bool special = false;
    vm::CellSlice mcs;
    try {
      mcs = vm::load_cell_slice_special(msg_cell, special);
    } catch (...) {
      return td::Status::Error("lz packet: bad message cell");
    }
    unsigned n_bits = mcs.size();
    size_t n_bytes = (n_bits + 7) / 8;
    std::string buf(n_bytes, '\0');
    if (n_bits > 0) {
      td::BitPtr(reinterpret_cast<unsigned char *>(buf.data())).copy_from(mcs.data_bits(), n_bits);
    }
    static const char hexd[] = "0123456789abcdef";
    message = "0x";
    for (char cbyte : buf) {
      unsigned char b = static_cast<unsigned char>(cbyte);
      message += hexd[b >> 4];
      message += hexd[b & 0xF];
    }
  }
  if (!ps.cs.have(64)) {
    return td::Status::Error("lz packet: nonce underflow");
  }
  auto nonce = ps.cs.fetch_ulong(64);
  TRY_RESULT(guid, lz_hex_min(ps.cs, 256));
  TRY_RESULT(rem, pslice_to_cell(ps));
  Value::Fields f;
  f.emplace_back("b", Value::make_cell(c));
  f.emplace_back("s", Value::make_cell(std::move(rem)));
  f.emplace_back("header_info", Value::make_int(std::move(hi)));
  f.emplace_back("header_filler", Value::make_int(std::move(hf)));
  f.emplace_back("path", std::move(path));
  f.emplace_back("message", Value::make_str(std::move(message)));
  f.emplace_back("nonce", Value::make_int(td::make_refint(nonce)));
  f.emplace_back("guid", Value::make_str(std::move(guid)));
  return Value::make_obj(std::move(f));
}

}  // namespace

td::Result<Value> parse_layerzero_oapp_execute_callback(const td::Ref<vm::Cell> &body) {
  // self.cell = slice.to_cell() on the FRESH body slice — pytoniq rebuilds the
  // cell; hash-identical to the body root, but rebuilt to mirror the semantics.
  PSlice ps = pslice_from_cell(body);
  TRY_RESULT(cell0, pslice_to_cell(ps));
  if (ps.refs.empty()) {
    return td::Status::Error("lz callback: refs[0] missing");
  }
  PSlice r0 = pslice_from_cell(ps.refs[0]);
  if (r0.refs.empty()) {
    return td::Status::Error("lz callback: refs[0].refs[0] missing");
  }
  TRY_RESULT(packet, lz_parse_packet(r0.refs[0]));
  Value::Fields f;
  f.emplace_back("cell", Value::make_cell(std::move(cell0)));
  f.emplace_back("packet", std::move(packet));
  return Value::make_obj(std::move(f));
}

}  // namespace mch
