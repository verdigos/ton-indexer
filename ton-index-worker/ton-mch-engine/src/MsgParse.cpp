// TL-B message-parsing adapters (see MsgParse.h for the semantics contract).
//
// Every quirk of the Python reference parsers is reproduced deliberately:
//  - pytoniq Slice.copy() RESETS ref consumption, so JettonTransfer's INLINE
//    forward_payload sees ALL body refs from index 0 (including an already
//    consumed custom_payload ref) — both in to_cell() and in the snake walk;
//  - JettonTransfer.sum_type is a conditionally-present key (Python only sets
//    the attribute on some paths; notably NOT on stonfi-swap success and NOT
//    when the body has no payload bits);
//  - load_address: addr_none -> null, addr_extern -> "extern:<len>:<value>"
//    Str (len 0 fails: pytoniq ba2int on an empty bitarray raises), addr_var
//    fails, anycast is consumed but NOT applied (pytoniq stores it without
//    rewriting the address prefix, unlike convert::to_raw_address);
//  - load_coins with len nibble 0 yields 0 (not null);
//  - payload_sum_type is Python hex(): lowercase, no zero padding.
#include "MsgParse.h"

#include "TraceLoader.h"

#include "crypto/block/block-parse.h"
#include "common/refint.h"
#include "td/utils/base64.h"
#include "vm/boc.h"
#include "vm/cellslice.h"
#include "vm/cells/CellBuilder.h"

#include "generated/mch-msgs-tlb.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace mch {

namespace {

std::string hex_upper(const unsigned char *p, size_t n) {
  static const char *digits = "0123456789ABCDEF";
  std::string out(n * 2, '0');
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = digits[p[i] >> 4];
    out[2 * i + 1] = digits[p[i] & 0xF];
  }
  return out;
}

td::RefInt256 refint_u64(unsigned long long v) {
  auto hi = td::make_refint(static_cast<long long>(v >> 32));
  return (hi << 32) + td::make_refint(static_cast<long long>(v & 0xFFFFFFFFULL));
}

// Serialized BOC bytes, mirroring pytoniq Cell.to_boc(hash_crc32=True)
// BYTE-FOR-BYTE (B0): pytoniq container layout differs from td's on
// multi-ref trees, so the pytoniq-order writer is used, not std_boc_serialize.
td::Result<std::string> boc_bytes(const td::Ref<vm::Cell> &c) {
  return pytoniq_to_boc(c, /*hash_crc32=*/true);
}

// pytoniq Slice.load_address(). Advances `cs`. Error == the Python parser
// raising (whole message parse fails, or the enclosing try downgrades it).
td::Result<Value> load_address_py(vm::CellSlice &cs) {
  if (!cs.have(2)) {
    return td::Status::Error("address: underflow");
  }
  auto tag = cs.fetch_ulong(2);
  if (tag == 0) {
    return Value::null();  // pytoniq returns None for addr_none
  }
  if (tag == 1) {
    if (!cs.have(9)) {
      return td::Status::Error("address: extern underflow");
    }
    int len = static_cast<int>(cs.fetch_ulong(9));
    if (len == 0) {
      return td::Status::Error("address: extern len 0");  // ba2int('') raises
    }
    if (!cs.have(len)) {
      return td::Status::Error("address: extern underflow");
    }
    auto v = cs.fetch_int256(len, false);
    if (v.is_null()) {
      return td::Status::Error("address: extern fetch failed");
    }
    return Value::make_str("extern:" + std::to_string(len) + ":" + v->to_dec_string());
  }
  if (tag == 3) {
    return td::Status::Error("address: addr_var unsupported");  // pytoniq raises
  }
  if (!cs.have(1)) {
    return td::Status::Error("address: underflow");
  }
  if (cs.fetch_ulong(1)) {  // anycast: consumed, never applied (pytoniq)
    if (!cs.have(5)) {
      return td::Status::Error("address: anycast underflow");
    }
    int depth = static_cast<int>(cs.fetch_ulong(5));
    if (depth < 1) {
      return td::Status::Error("address: anycast depth 0");  // pytoniq raises
    }
    if (!cs.have(depth) || !cs.advance(depth)) {
      return td::Status::Error("address: anycast underflow");
    }
  }
  if (!cs.have(8 + 256)) {
    return td::Status::Error("address: std underflow");
  }
  int wc = static_cast<int>(cs.fetch_long(8));
  unsigned char buf[32];
  if (!cs.fetch_bytes(buf, 32)) {
    return td::Status::Error("address: std fetch failed");
  }
  return Value::make_account_raw(std::to_string(wc) + ":" + hex_upper(buf, 32));
}

td::Result<Value> addr_value(td::Ref<vm::CellSlice> csr) {
  vm::CellSlice cs{*csr};
  return load_address_py(cs);
}

// pytoniq Slice.load_coins(): len nibble 0 -> 0. Advances `cs`.
td::Result<td::RefInt256> load_coins_py(vm::CellSlice &cs) {
  if (!cs.have(4)) {
    return td::Status::Error("coins: underflow");
  }
  int len = static_cast<int>(cs.fetch_ulong(4));
  if (len == 0) {
    return td::make_refint(0);
  }
  if (!cs.have(len * 8)) {
    return td::Status::Error("coins: underflow");
  }
  auto v = cs.fetch_int256(len * 8, false);
  if (v.is_null()) {
    return td::Status::Error("coins: fetch failed");
  }
  return v;
}

td::Result<td::RefInt256> var_uint16(const td::Ref<vm::CellSlice> &csr) {
  auto v = block::tlb::t_VarUInteger_16.as_integer(csr);
  if (v.is_null()) {
    return td::Status::Error("VarUInteger16: bad slice");
  }
  return v;
}

// pytoniq-Slice stand-in: `cs` carries the bit cursor, `refs`/`off` carry the
// ref list SEPARATELY, because pytoniq's copy()/to_cell()/snake semantics do
// not always agree with the CellSlice's own ref cursor (see file header).
struct PSlice {
  vm::CellSlice cs;
  std::vector<td::Ref<vm::Cell>> refs;
  size_t off{0};
};

PSlice pslice_from_cell(const td::Ref<vm::Cell> &c) {
  PSlice ps;
  bool special = false;
  ps.cs = vm::load_cell_slice_special(c, special);
  for (unsigned i = 0; i < ps.cs.size_refs(); i++) {
    ps.refs.push_back(ps.cs.prefetch_ref(i));
  }
  return ps;
}

// pytoniq Slice.to_cell(): Cell(remaining bits, refs[off:]).
td::Result<td::Ref<vm::Cell>> pslice_to_cell(const PSlice &ps) {
  try {
    vm::CellBuilder cb;
    cb.store_bits(ps.cs.data_bits(), ps.cs.size());
    for (size_t i = ps.off; i < ps.refs.size(); i++) {
      cb.store_ref(ps.refs[i]);
    }
    return cb.finalize();
  } catch (...) {
    return td::Status::Error("to_cell failed");
  }
}

// pytoniq Slice.load_snake_bytes(): byte-aligned, <=1 ref per link.
td::Result<std::string> load_snake_bytes(PSlice ps) {
  std::string out;
  for (;;) {
    if (ps.cs.size() % 8 != 0) {
      return td::Status::Error("snake: not byte-aligned");
    }
    size_t nrefs = ps.refs.size() - ps.off;
    if (nrefs > 1) {
      return td::Status::Error("snake: >1 ref");
    }
    size_t n = ps.cs.size() / 8;
    if (n > 0) {
      std::string chunk(n, '\0');
      if (!ps.cs.fetch_bytes(reinterpret_cast<unsigned char *>(chunk.data()), static_cast<int>(n))) {
        return td::Status::Error("snake: fetch failed");
      }
      out += chunk;
    }
    if (nrefs == 0) {
      return out;
    }
    ps = pslice_from_cell(ps.refs[ps.off]);
  }
}

struct BodyCtx {
  vm::CellSlice cs;
  std::vector<td::Ref<vm::Cell>> all_refs;  // body refs from index 0 (copy() quirk)
};

td::Result<BodyCtx> open_body(const td::Ref<vm::Cell> &body) {
  if (body.is_null()) {
    return td::Status::Error("null body");
  }
  BodyCtx ctx;
  bool special = false;
  try {
    ctx.cs = vm::load_cell_slice_special(body, special);
  } catch (...) {
    return td::Status::Error("bad body cell");
  }
  for (unsigned i = 0; i < ctx.cs.size_refs(); i++) {
    ctx.all_refs.push_back(ctx.cs.prefetch_ref(i));
  }
  return ctx;
}

// stonfi swap body inside JettonTransfer.forward_payload (sum type 0x25938561).
td::Result<Value> parse_stonfi_swap(vm::CellSlice &cs) {
  TRY_RESULT(jw, load_address_py(cs));
  TRY_RESULT(min_amount, load_coins_py(cs));
  TRY_RESULT(user, load_address_py(cs));
  Value::Fields f;
  f.emplace_back("jetton_wallet", std::move(jw));
  f.emplace_back("min_amount", Value::make_int(std::move(min_amount)));
  f.emplace_back("user_address", std::move(user));
  return Value::make_dict(std::move(f));
}

// --- per-family adapters ----------------------------------------------------

td::Result<Value> parse_jetton_transfer(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  mchmsg::gen::MchJettonTransfer::Record rec;
  if (!mchmsg::gen::t_MchJettonTransfer.unpack(cs, rec)) {
    return td::Status::Error("tlb: transfer unpack failed");
  }
  TRY_RESULT(amount, var_uint16(rec.amount));
  TRY_RESULT(destination, addr_value(rec.destination));
  TRY_RESULT(response, addr_value(rec.response));
  td::Ref<vm::Cell> cp;
  if (!rec.custom_payload.write().fetch_maybe_ref(cp)) {
    return td::Status::Error("transfer: bad custom_payload");
  }
  Value custom_payload = Value::null();
  if (cp.not_null()) {
    TRY_RESULT(cp_boc, boc_bytes(cp));
    custom_payload = Value::make_bytes(std::move(cp_boc));
  }
  TRY_RESULT(forward_amount, var_uint16(rec.forward_amount));

  Value forward_payload = Value::null();
  Value comment = Value::null();
  Value payload_sum_type = Value::null();
  Value stonfi_swap_body = Value::null();
  bool encrypted_comment = false;
  bool has_sum_type = false;
  std::string sum_type;

  if (cs.size() > 0) {
    bool in_ref = cs.fetch_ulong(1) != 0;
    PSlice ps;
    if (in_ref) {
      if (cs.size_refs() == 0) {
        return td::Status::Error("transfer: forward_payload ref missing");
      }
      ps = pslice_from_cell(cs.fetch_ref());
    } else {
      // pytoniq boc.copy(): remaining bits, but refs reset to the FULL body
      // ref list with offset 0 (Slice.copy() does not preserve ref_offset).
      ps.cs = cs;
      ps.refs = ctx.all_refs;
      ps.off = 0;
    }
    // _load_forward_payload
    if (ps.cs.size() > 0) {
      TRY_RESULT(fp_cell, pslice_to_cell(ps));
      TRY_RESULT(fp_boc, boc_bytes(fp_cell));
      forward_payload = Value::make_bytes(std::move(fp_boc));
      if (ps.cs.size() < 32) {
        has_sum_type = true;
        sum_type = "Unknown";
      } else {
        auto st = static_cast<td::uint32>(ps.cs.fetch_ulong(32));
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%x", st);  // Python hex()
        payload_sum_type = Value::make_str(buf);
        if (st == 0) {
          has_sum_type = true;
          sum_type = "TextComment";
          auto r = load_snake_bytes(ps);
          if (r.is_ok()) {
            comment = Value::make_bytes(r.move_as_ok());
          } else {
            sum_type = "Unknown";
          }
        } else if (st == 0x2167da4b) {
          has_sum_type = true;
          sum_type = "EncryptedTextComment";
          auto r = load_snake_bytes(ps);
          if (r.is_ok()) {
            comment = Value::make_bytes(r.move_as_ok());
            encrypted_comment = true;
          } else {
            sum_type = "Unknown";
          }
        } else if (st == 0x25938561) {
          auto r = parse_stonfi_swap(ps.cs);
          if (r.is_ok()) {
            stonfi_swap_body = r.move_as_ok();
            // Python does NOT set sum_type on this path.
          } else {
            has_sum_type = true;
            sum_type = "Unknown";
          }
        } else {
          has_sum_type = true;
          sum_type = "Unknown";
        }
      }
    }
  }

  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(rec.query_id)));
  f.emplace_back("amount", Value::make_int(std::move(amount)));
  f.emplace_back("destination", std::move(destination));
  f.emplace_back("response", std::move(response));
  f.emplace_back("custom_payload", std::move(custom_payload));
  f.emplace_back("forward_amount", Value::make_int(std::move(forward_amount)));
  f.emplace_back("comment", std::move(comment));
  f.emplace_back("encrypted_comment", Value::make_bool(encrypted_comment));
  f.emplace_back("payload_sum_type", std::move(payload_sum_type));
  f.emplace_back("stonfi_swap_body", std::move(stonfi_swap_body));
  f.emplace_back("forward_payload", std::move(forward_payload));
  if (has_sum_type) {
    f.emplace_back("sum_type", Value::make_str(std::move(sum_type)));
  }
  return Value::make_obj(std::move(f));
}

td::Result<Value> parse_jetton_internal_transfer(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  mchmsg::gen::MchJettonInternalTransfer::Record rec;
  if (!mchmsg::gen::t_MchJettonInternalTransfer.unpack(ctx.cs, rec)) {
    return td::Status::Error("tlb: internal_transfer unpack failed");
  }
  TRY_RESULT(amount, var_uint16(rec.amount));
  TRY_RESULT(from_address, addr_value(rec.from_address));
  TRY_RESULT(response_address, addr_value(rec.response_address));
  TRY_RESULT(forward_ton_amount, var_uint16(rec.forward_ton_amount));
  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(rec.query_id)));
  f.emplace_back("amount", Value::make_int(std::move(amount)));
  f.emplace_back("from_address", std::move(from_address));
  f.emplace_back("response_address", std::move(response_address));
  f.emplace_back("forward_ton_amount", Value::make_int(std::move(forward_ton_amount)));
  return Value::make_obj(std::move(f));
}

td::Result<Value> parse_jetton_burn(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  mchmsg::gen::MchJettonBurn::Record rec;
  if (!mchmsg::gen::t_MchJettonBurn.unpack(ctx.cs, rec)) {
    return td::Status::Error("tlb: burn unpack failed");
  }
  TRY_RESULT(amount, var_uint16(rec.amount));
  TRY_RESULT(response_destination, addr_value(rec.response_destination));
  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(rec.query_id)));
  f.emplace_back("amount", Value::make_int(std::move(amount)));
  f.emplace_back("response_destination", std::move(response_destination));
  return Value::make_obj(std::move(f));
}

td::Result<Value> parse_jetton_notify(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  mchmsg::gen::MchJettonNotify::Record rec;
  if (!mchmsg::gen::t_MchJettonNotify.unpack(cs, rec)) {
    return td::Status::Error("tlb: notify unpack failed");
  }
  TRY_RESULT(jetton_amount, var_uint16(rec.jetton_amount));
  TRY_RESULT(from_user, addr_value(rec.from_user));
  // load_maybe_ref: the Maybe bit itself is REQUIRED (pytoniq raises on empty).
  if (!cs.have(1)) {
    return td::Status::Error("notify: no forward_payload bit");
  }
  Value forward_payload_cell = Value::null();
  if (cs.fetch_ulong(1)) {
    if (cs.size_refs() == 0) {
      return td::Status::Error("notify: forward_payload ref missing");
    }
    forward_payload_cell = Value::make_cell(cs.fetch_ref());
  } else if (cs.size() > 0) {
    // Python keeps the body SLICE; the twin renders it via to_cell(), so we
    // build Cell(remaining bits, remaining refs) here.
    PSlice ps;
    ps.cs = cs;
    for (unsigned i = 0; i < cs.size_refs(); i++) {
      ps.refs.push_back(cs.prefetch_ref(i));
    }
    TRY_RESULT(c, pslice_to_cell(ps));
    forward_payload_cell = Value::make_cell(std::move(c));
  }
  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(rec.query_id)));
  f.emplace_back("jetton_amount", Value::make_int(std::move(jetton_amount)));
  f.emplace_back("from_user", std::move(from_user));
  f.emplace_back("forward_payload_cell", std::move(forward_payload_cell));
  return Value::make_obj(std::move(f));
}

td::Result<Value> parse_jetton_mint(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  mchmsg::gen::MchJettonMint::Record rec;
  if (!mchmsg::gen::t_MchJettonMint.unpack(ctx.cs, rec)) {
    return td::Status::Error("tlb: mint unpack failed");
  }
  TRY_RESULT(to_address, addr_value(rec.to_address));
  TRY_RESULT(ton_amount, var_uint16(rec.ton_amount));
  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(rec.query_id)));
  f.emplace_back("to_address", std::move(to_address));
  f.emplace_back("ton_amount", Value::make_int(std::move(ton_amount)));
  return Value::make_obj(std::move(f));
}

td::Result<Value> parse_minter_jetton_mint(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  mchmsg::gen::MchMinterJettonMint::Record rec;
  if (!mchmsg::gen::t_MchMinterJettonMint.unpack(ctx.cs, rec)) {
    return td::Status::Error("tlb: minter mint unpack failed");
  }
  TRY_RESULT(to_address, addr_value(rec.to_address));
  TRY_RESULT(ton_amount, var_uint16(rec.ton_amount));
  bool special = false;
  vm::CellSlice ms;
  try {
    ms = vm::load_cell_slice_special(rec.master_msg, special);
  } catch (...) {
    return td::Status::Error("minter mint: bad master_msg");
  }
  if (!ms.have(32) || !ms.advance(32) || !ms.have(64)) {
    return td::Status::Error("minter mint: master_msg underflow");
  }
  auto master_query_id = ms.fetch_ulong(64);
  TRY_RESULT(master_amount, load_coins_py(ms));
  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(rec.query_id)));
  f.emplace_back("to_address", std::move(to_address));
  f.emplace_back("ton_amount", Value::make_int(std::move(ton_amount)));
  f.emplace_back("master_msg", Value::make_cell(rec.master_msg));
  f.emplace_back("master_msg_query_id", Value::make_int(refint_u64(master_query_id)));
  f.emplace_back("master_msg_jetton_amount", Value::make_int(std::move(master_amount)));
  return Value::make_obj(std::move(f));
}

// Python's JettonBurnNotification has no __init__(Slice): _soft_parse's
// cls(body) raises TypeError, i.e. the parse ALWAYS fails.
td::Result<Value> parse_never(const td::Ref<vm::Cell> &) {
  return td::Status::Error("message type is not constructible (Python parity)");
}

// --- Phase C adapters (all HAND-WRITTEN: each family carries a pytoniq quirk
// a clean TL-B constructor cannot express — dns's dead-value_slice chunk walk,
// vesting's inner msg-info descent, pton's to_cell() remainder, coffee's
// custom asset tag; dedust payout is trivial and stays consistent) ----------

// str.decode() parity: Python's strict utf-8 decode raises on invalid bytes,
// failing the whole parse.
bool valid_utf8(const std::string &s) {
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    int n;
    if (c < 0x80) {
      n = 0;
    } else if ((c & 0xE0) == 0xC0 && c >= 0xC2) {
      n = 1;
    } else if ((c & 0xF0) == 0xE0) {
      n = 2;
    } else if ((c & 0xF8) == 0xF0 && c <= 0xF4) {
      n = 3;
    } else {
      return false;
    }
    if (i + n >= s.size()) {
      return false;
    }
    for (int k = 1; k <= n; k++) {
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
        return false;
      }
    }
    // Reject overlongs / surrogates / >U+10FFFF like Python's strict decoder.
    if (n == 2) {
      unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
      if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) {
        return false;
      }
    }
    if (n == 3) {
      unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
      if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) {
        return false;
      }
    }
    i += n + 1;
  }
  return true;
}

td::Result<std::string> fetch_bytes_str(vm::CellSlice &cs, int n) {
  if (!cs.have(n * 8)) {
    return td::Status::Error("bytes: underflow");
  }
  std::string out(static_cast<size_t>(n), '\0');
  if (!cs.fetch_bytes(reinterpret_cast<unsigned char *>(out.data()), n)) {
    return td::Status::Error("bytes: fetch failed");
  }
  return out;
}

// AccountId(load_address()) as dns's value dicts wrap it: None -> AccountId(None)
// (addr_none Account, NOT null); an extern address string raises in the
// AccountId ctor -> the whole parse fails.
td::Result<Value> account_id_of(Value addr) {
  if (addr.is_null()) {
    return Value::make_account_none();
  }
  if (addr.t == VType::Str) {
    return td::Status::Error("AccountId(extern) raises");
  }
  return addr;
}

// ChangeDnsRecordMessage (messages/dns.py). The DNSText chunk walk reproduces
// the Python loop verbatim: every chunk's length+text is read from the SAME
// value slice; the first extra chunk consumes one of ITS refs into the dead
// value_slice variable; a third chunk calls load_ref on a Cell and always
// raises (-> parse fail).
td::Result<Value> parse_change_dns(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64) || !cs.advance(32)) {
    return td::Status::Error("dns: header underflow");
  }
  auto query_id = cs.fetch_ulong(64);
  TRY_RESULT(key, fetch_bytes_str(cs, 32));
  bool has_value = cs.size_refs() > 0;
  Value value = Value::null();
  if (has_value) {
    vm::CellSlice vs;
    bool special = false;
    try {
      vs = vm::load_cell_slice_special(cs.fetch_ref(), special);
    } catch (...) {
      return td::Status::Error("dns: bad value cell");
    }
    if (!vs.have(16)) {
      return td::Status::Error("dns: schema underflow");
    }
    auto schema = static_cast<unsigned>(vs.fetch_ulong(16));
    Value::Fields vf;
    if (schema == 0xba93) {
      TRY_RESULT(addr, load_address_py(vs));
      TRY_RESULT(acc, account_id_of(std::move(addr)));
      vf.emplace_back("schema", Value::make_str("DNSNextResolver"));
      vf.emplace_back("address", std::move(acc));
    } else if (schema == 0xad01) {
      TRY_RESULT(adnl, fetch_bytes_str(vs, 32));
      if (!vs.have(8)) {
        return td::Status::Error("dns: adnl flags underflow");
      }
      vf.emplace_back("schema", Value::make_str("DNSAdnlAddress"));
      vf.emplace_back("address", Value::make_bytes(std::move(adnl)));
      vf.emplace_back("flags", Value::make_int64(static_cast<std::int64_t>(vs.fetch_ulong(8))));
    } else if (schema == 0x9fd3) {
      TRY_RESULT(addr, load_address_py(vs));
      TRY_RESULT(acc, account_id_of(std::move(addr)));
      if (!vs.have(8)) {
        return td::Status::Error("dns: smc flags underflow");
      }
      vf.emplace_back("schema", Value::make_str("DNSSmcAddress"));
      vf.emplace_back("address", std::move(acc));
      vf.emplace_back("flags", Value::make_int64(static_cast<std::int64_t>(vs.fetch_ulong(8))));
    } else if (schema == 0x7473) {
      TRY_RESULT(bag, fetch_bytes_str(vs, 32));
      vf.emplace_back("schema", Value::make_str("DNSStorageAddress"));
      vf.emplace_back("address", Value::make_bytes(std::move(bag)));
    } else if (schema == 0x1eda) {
      if (!vs.have(8)) {
        return td::Status::Error("dns: chunk count underflow");
      }
      int chunks = static_cast<int>(vs.fetch_ulong(8));
      std::string dns_text;
      bool value_slice_is_cell = false;
      while (chunks > 0) {
        if (!vs.have(8)) {
          return td::Status::Error("dns: chunk len underflow");
        }
        int len = static_cast<int>(vs.fetch_ulong(8));
        TRY_RESULT(chunk, fetch_bytes_str(vs, len));
        if (!valid_utf8(chunk)) {
          return td::Status::Error("dns: chunk not utf-8");
        }
        dns_text += chunk;
        chunks--;
        if (chunks > 0) {
          if (value_slice_is_cell) {
            // Python: Cell.load_ref does not exist -> AttributeError.
            return td::Status::Error("dns: load_ref on a cell");
          }
          if (vs.size_refs() == 0) {
            return td::Status::Error("dns: chunk ref missing");
          }
          vs.fetch_ref();  // consumed into the dead value_slice variable
          value_slice_is_cell = true;
        }
      }
      vf.emplace_back("schema", Value::make_str("DNSText"));
      vf.emplace_back("dns_text", Value::make_str(std::move(dns_text)));
    } else {
      vf.emplace_back("schema", Value::make_str("Unknown"));
    }
    value = Value::make_dict(std::move(vf));
  }
  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(query_id)));
  f.emplace_back("key", Value::make_bytes(std::move(key)));
  f.emplace_back("has_value", Value::make_bool(has_value));
  f.emplace_back("value", std::move(value));
  return Value::make_obj(std::move(f));
}

// VestingSendMessage (messages/vesting.py): the inner message cell's
// int_msg_info header is fully deserialized (any failure fails the parse).
// Deviation (documented): a present extra-currency dict ref is consumed but
// not deep-validated (Python's HashMap.parse would validate; vesting inner
// messages never carry extra currencies in practice).
td::Result<Value> parse_vesting_send_message(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64 + 8) || !cs.advance(32)) {
    return td::Status::Error("vesting: header underflow");
  }
  auto query_id = cs.fetch_ulong(64);
  auto send_mode = cs.fetch_ulong(8);
  if (cs.size_refs() == 0) {
    return td::Status::Error("vesting: message_cell ref missing");
  }
  td::Ref<vm::Cell> message_cell = cs.fetch_ref();

  vm::CellSlice ms;
  bool special = false;
  try {
    ms = vm::load_cell_slice_special(message_cell, special);
  } catch (...) {
    return td::Status::Error("vesting: bad message cell");
  }
  // InternalMsgInfo.deserialize: tag bit 0, ihr_disabled/bounce/bounced,
  // src, dest, value (grams + extra dict), ihr_fee, fwd_fee, lt, at.
  if (!ms.have(4)) {
    return td::Status::Error("vesting: msg info underflow");
  }
  if (ms.fetch_ulong(1) != 0) {
    return td::Status::Error("vesting: not int_msg_info");
  }
  ms.advance(3);
  TRY_RESULT(src, load_address_py(ms));
  (void)src;
  TRY_RESULT(dest, load_address_py(ms));
  TRY_RESULT(grams, load_coins_py(ms));
  if (!ms.have(1)) {
    return td::Status::Error("vesting: extra dict underflow");
  }
  if (ms.fetch_ulong(1)) {
    if (ms.size_refs() == 0) {
      return td::Status::Error("vesting: extra dict ref missing");
    }
    ms.fetch_ref();
  }
  TRY_RESULT(ihr_fee, load_coins_py(ms));
  (void)ihr_fee;
  TRY_RESULT(fwd_fee, load_coins_py(ms));
  (void)fwd_fee;
  if (!ms.have(64 + 32)) {
    return td::Status::Error("vesting: lt/at underflow");
  }
  ms.advance(64 + 32);

  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(query_id)));
  f.emplace_back("send_mode", Value::make_int64(static_cast<std::int64_t>(send_mode)));
  f.emplace_back("message_cell", Value::make_cell(std::move(message_cell)));
  f.emplace_back("message_destination", std::move(dest));
  f.emplace_back("message_value", Value::make_int(std::move(grams)));
  return Value::make_obj(std::move(f));
}

// PTonTransfer (messages/swaps.py): forward_payload = maybe-ref Cell, else the
// slice REMAINDER as a cell when refs remain (pytoniq body.to_cell(): remaining
// bits + refs FROM the current ref cursor — NOT the copy() reset).
td::Result<Value> parse_pton_transfer(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64) || !cs.advance(32)) {
    return td::Status::Error("pton: header underflow");
  }
  auto query_id = cs.fetch_ulong(64);
  TRY_RESULT(ton_amount, load_coins_py(cs));
  TRY_RESULT(refund_address, load_address_py(cs));
  if (!cs.have(1)) {
    return td::Status::Error("pton: forward_payload bit missing");
  }
  Value forward_payload = Value::null();
  if (cs.fetch_ulong(1)) {
    if (cs.size_refs() == 0) {
      return td::Status::Error("pton: forward_payload ref missing");
    }
    forward_payload = Value::make_cell(cs.fetch_ref());
  } else if (cs.size_refs() > 0) {
    try {
      vm::CellBuilder cb;
      cb.store_bits(cs.data_bits(), cs.size());
      for (unsigned i = 0; i < cs.size_refs(); i++) {
        cb.store_ref(cs.prefetch_ref(i));
      }
      forward_payload = Value::make_cell(cb.finalize());
    } catch (...) {
      return td::Status::Error("pton: to_cell failed");
    }
  }
  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(query_id)));
  f.emplace_back("ton_amount", Value::make_int(std::move(ton_amount)));
  f.emplace_back("refund_address", std::move(refund_address));
  f.emplace_back("forward_payload", std::move(forward_payload));
  return Value::make_obj(std::move(f));
}

// DedustPayoutFromPool (messages/swaps.py).
td::Result<Value> parse_dedust_payout_from_pool(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64) || !cs.advance(32)) {
    return td::Status::Error("dedust payout: header underflow");
  }
  auto query_id = cs.fetch_ulong(64);
  if (cs.size_refs() == 0) {
    return td::Status::Error("dedust payout: proof ref missing");
  }
  td::Ref<vm::Cell> proof = cs.fetch_ref();
  TRY_RESULT(amount, load_coins_py(cs));
  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(query_id)));
  f.emplace_back("proof", Value::make_cell(std::move(proof)));
  f.emplace_back("amount", Value::make_int(std::move(amount)));
  return Value::make_obj(std::move(f));
}

// --- Phase D adapters (EVAA withdraw family; messages/evaa.py) --------------

// withdraw_master#00000002: query_id u64, asset_id u256, amount u64, recipient.
td::Result<Value> parse_evaa_withdraw_master(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64 + 256 + 64) || !cs.advance(32)) {
    return td::Status::Error("evaa master: underflow");
  }
  auto query_id = cs.fetch_ulong(64);
  auto asset_id = cs.fetch_int256(256, false);
  if (asset_id.is_null()) {
    return td::Status::Error("evaa master: asset_id fetch failed");
  }
  auto amount = cs.fetch_ulong(64);
  TRY_RESULT(recipient, load_address_py(cs));
  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(query_id)));
  f.emplace_back("asset_id", Value::make_int(std::move(asset_id)));
  f.emplace_back("amount", Value::make_int(refint_u64(amount)));
  f.emplace_back("recipient_address", std::move(recipient));
  return Value::make_obj(std::move(f));
}

// withdraw_collateralized#00000211: query_id u64, owner addr, asset_id u256,
// withdraw_amount_current u64 (rest skipped like the Python parser).
td::Result<Value> parse_evaa_withdraw_collateralized(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64) || !cs.advance(32)) {
    return td::Status::Error("evaa coll: underflow");
  }
  auto query_id = cs.fetch_ulong(64);
  TRY_RESULT(owner, load_address_py(cs));
  if (!cs.have(256 + 64)) {
    return td::Status::Error("evaa coll: underflow");
  }
  auto asset_id = cs.fetch_int256(256, false);
  if (asset_id.is_null()) {
    return td::Status::Error("evaa coll: asset_id fetch failed");
  }
  auto amount = cs.fetch_ulong(64);
  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(query_id)));
  f.emplace_back("owner_address", std::move(owner));
  f.emplace_back("asset_id", Value::make_int(std::move(asset_id)));
  f.emplace_back("withdraw_amount_current", Value::make_int(refint_u64(amount)));
  return Value::make_obj(std::move(f));
}

// EvaaWithdrawFailExcess: opcode-tagged reason; an unknown opcode raises in
// Python (ValueError) -> soft-parse failure.
td::Result<Value> parse_evaa_withdraw_fail_excess(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32)) {
    return td::Status::Error("evaa fail excess: underflow");
  }
  auto op = static_cast<td::uint32>(cs.fetch_ulong(32));
  const char *reason;
  if (op == 0x21e6) {
    reason = "withdraw_locked_excess";
  } else if (op == 0x21e7) {
    reason = "withdraw_not_collateralized_excess";
  } else if (op == 0x21e8) {
    reason = "withdraw_missing_prices_excess";
  } else if (op == 0x21ec) {
    reason = "withdraw_execution_crashed";
  } else {
    return td::Status::Error("evaa fail excess: unknown opcode");
  }
  Value::Fields f;
  f.emplace_back("opcode", Value::make_int64(static_cast<std::int64_t>(op)));
  f.emplace_back("reason", Value::make_str(reason));
  return Value::make_obj(std::move(f));
}

// CoffeeCreateVault (messages/coffee.py + load_coffee_asset): native/jetton
// asset tags; extra$10 and the invalid tag raise in Python -> parse fail.
td::Result<Value> parse_coffee_create_vault(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32 + 64) || !cs.advance(32)) {
    return td::Status::Error("coffee vault: header underflow");
  }
  auto query_id = cs.fetch_ulong(64);
  if (!cs.have(2)) {
    return td::Status::Error("coffee vault: asset tag underflow");
  }
  auto tag = cs.fetch_ulong(2);
  Value asset;
  if (tag == 0) {
    asset = Value::make_asset_ton();
  } else if (tag == 1) {
    if (!cs.have(8)) {
      return td::Status::Error("coffee vault: asset chain underflow");
    }
    int chain = static_cast<int>(cs.fetch_ulong(8));
    if (!cs.have(256)) {
      return td::Status::Error("coffee vault: asset hash underflow");
    }
    unsigned char buf[32];
    if (!cs.fetch_bytes(buf, 32)) {
      return td::Status::Error("coffee vault: asset hash fetch failed");
    }
    asset = Value::make_asset_jetton(std::to_string(chain) + ":" + hex_upper(buf, 32));
  } else {
    // extra$10 -> NotImplementedError; anything else -> ValueError.
    return td::Status::Error("coffee vault: unsupported asset tag");
  }
  Value::Fields f;
  f.emplace_back("query_id", Value::make_int(refint_u64(query_id)));
  f.emplace_back("asset", std::move(asset));
  return Value::make_obj(std::move(f));
}

// messages/swaps.py load_asset: kind:uint4 (0 -> TON) else wc:uint8 + 32 bytes.
// Asset(False, Address((wc, bytes))) -> canonical "wc:HEX" upper (AccountId).
td::Result<Value> load_dedust_asset(vm::CellSlice &cs) {
  if (!cs.have(4)) {
    return td::Status::Error("dedust asset: kind underflow");
  }
  auto kind = cs.fetch_ulong(4);
  if (kind == 0) {
    return Value::make_asset_ton();
  }
  if (!cs.have(8 + 256)) {
    return td::Status::Error("dedust asset: jetton underflow");
  }
  int wc = static_cast<int>(cs.fetch_ulong(8));
  unsigned char buf[32];
  if (!cs.fetch_bytes(buf, 32)) {
    return td::Status::Error("dedust asset: hash fetch failed");
  }
  return Value::make_asset_jetton(std::to_string(wc) + ":" + hex_upper(buf, 32));
}

// DedustSwapNotification (messages/swaps.py). Parses the full body incl. the
// ref (Python load_ref() raises when absent -> parse fails -> host fn rejects).
td::Result<Value> parse_dedust_swap_notification(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  if (!cs.have(32) || !cs.advance(32)) {
    return td::Status::Error("dedust notify: opcode underflow");
  }
  TRY_RESULT(asset_in, load_dedust_asset(cs));
  TRY_RESULT(asset_out, load_dedust_asset(cs));
  TRY_RESULT(amount_in, load_coins_py(cs));
  TRY_RESULT(amount_out, load_coins_py(cs));
  if (cs.size_refs() == 0) {
    return td::Status::Error("dedust notify: ref missing");
  }
  bool special = false;
  vm::CellSlice ref;
  try {
    ref = vm::load_cell_slice_special(cs.fetch_ref(), special);
  } catch (...) {
    return td::Status::Error("dedust notify: bad ref cell");
  }
  TRY_RESULT(sender_address, load_address_py(ref));
  TRY_RESULT(ref_address, load_address_py(ref));
  TRY_RESULT(reserve_0, load_coins_py(ref));
  TRY_RESULT(reserve_1, load_coins_py(ref));
  Value::Fields f;
  f.emplace_back("asset_in", std::move(asset_in));
  f.emplace_back("asset_out", std::move(asset_out));
  f.emplace_back("amount_in", Value::make_int(std::move(amount_in)));
  f.emplace_back("amount_out", Value::make_int(std::move(amount_out)));
  f.emplace_back("sender_address", std::move(sender_address));
  f.emplace_back("ref_address", std::move(ref_address));
  f.emplace_back("reserve_0", Value::make_int(std::move(reserve_0)));
  f.emplace_back("reserve_1", Value::make_int(std::move(reserve_1)));
  return Value::make_obj(std::move(f));
}

// --- LayerZero (messages/layerzero.py:149-208, 1377-1383) --------------------
// LayerZeroOappExecuteCallback: cell = slice.to_cell(); packet =
// LayerZeroPacket(cell.refs[0].refs[0]). The twin dumps vars() recursively, so
// the Obj mirrors EVERY Python attribute, incl. b (the source cell), s (the
// slice AFTER all loads -> remainder rebuilt as a cell) and the asserted
// header ints. guid/src_oapp/dst_oapp are Python hex(): "0x" + lowercase
// MINIMAL digits (leading zeros stripped — unlike the 64-digit account form).
// message is '0x' + bits.tobytes().hex(): bit-level bytes, right-padded to a
// byte boundary, leading zeros KEPT.

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

}  // namespace

const std::map<std::string, MsgParserFn> &message_parsers() {
  static const std::map<std::string, MsgParserFn> parsers = {
      {"JettonTransfer", parse_jetton_transfer},
      {"JettonInternalTransfer", parse_jetton_internal_transfer},
      {"JettonBurn", parse_jetton_burn},
      {"JettonNotify", parse_jetton_notify},
      {"JettonMint", parse_jetton_mint},
      {"MinterJettonMint", parse_minter_jetton_mint},
      {"JettonBurnNotification", parse_never},
      {"ChangeDnsRecordMessage", parse_change_dns},
      {"VestingSendMessage", parse_vesting_send_message},
      {"PTonTransfer", parse_pton_transfer},
      {"DedustPayoutFromPool", parse_dedust_payout_from_pool},
      {"DedustSwapNotification", parse_dedust_swap_notification},
      {"CoffeeCreateVault", parse_coffee_create_vault},
      {"EvaaWithdrawMaster", parse_evaa_withdraw_master},
      {"EvaaWithdrawCollateralized", parse_evaa_withdraw_collateralized},
      {"EvaaWithdrawFailExcess", parse_evaa_withdraw_fail_excess},
      {"LayerZeroOappExecuteCallback", parse_layerzero_oapp_execute_callback},
  };
  return parsers;
}

td::Result<Value> parse_message_body(const std::string &type_name, const td::Ref<vm::Cell> &body) {
  auto it = message_parsers().find(type_name);
  if (it == message_parsers().end()) {
    return td::Status::Error(PSLICE() << "message type " << type_name << " is not registered");
  }
  try {
    return it->second(body);
  } catch (...) {
    return td::Status::Error("parse threw");
  }
}

td::Result<td::RefInt256> pyslice_load_coins(vm::CellSlice &cs) {
  return load_coins_py(cs);
}

// blocks/swaps.py _parse_dedust_steps: walk the SwapStep chain (already
// positioned past the sum-type/header). Each link: pool addr, 1 flag bit,
// coins, maybe-ref to the next step. Returns the pool addresses in walk order
// (AccountId(load_address()).as_str(); addr_none -> "addr_none").
td::Result<std::vector<std::string>> parse_dedust_steps(vm::CellSlice cs) {
  std::vector<std::string> steps;
  for (;;) {
    TRY_RESULT(addr, load_address_py(cs));  // pytoniq load_address
    std::string pool = (addr.t == VType::Account && !addr.addr_none) ? addr.str : "addr_none";
    if (!cs.have(1)) {
      return td::Status::Error("dedust steps: flag bit underflow");
    }
    cs.advance(1);                    // load_bit
    TRY_RESULT(_coins, load_coins_py(cs));
    (void)_coins;
    if (!cs.have(1)) {                // load_maybe_ref: the Maybe bit
      return td::Status::Error("dedust steps: maybe-ref bit underflow");
    }
    bool has_next = cs.fetch_ulong(1) != 0;
    steps.push_back(std::move(pool));
    if (!has_next) {
      return steps;
    }
    if (cs.size_refs() == 0) {
      return td::Status::Error("dedust steps: next-step ref missing");
    }
    bool special = false;
    try {
      cs = vm::load_cell_slice_special(cs.fetch_ref(), special);  // next_step.to_slice()
    } catch (...) {
      return td::Status::Error("dedust steps: bad next-step cell");
    }
  }
}

namespace {

// Python bytes.decode("utf-8", errors="backslashreplace") + .replace("\u0000", "").
std::string decode_backslashreplace_strip_nul(const std::string &raw) {
  std::string out;
  size_t i = 0;
  auto bad = [&out](unsigned char c) {
    static const char *hex = "0123456789abcdef";
    out += "\\x";
    out += hex[c >> 4];
    out += hex[c & 0xF];
  };
  while (i < raw.size()) {
    unsigned char c = static_cast<unsigned char>(raw[i]);
    int n;
    if (c < 0x80) {
      n = 0;
    } else if ((c & 0xE0) == 0xC0 && c >= 0xC2) {
      n = 1;
    } else if ((c & 0xF0) == 0xE0) {
      n = 2;
    } else if ((c & 0xF8) == 0xF0 && c <= 0xF4) {
      n = 3;
    } else {
      bad(c);
      i++;
      continue;
    }
    bool ok = i + n < raw.size();
    for (int k = 1; ok && k <= n; k++) {
      if ((static_cast<unsigned char>(raw[i + k]) & 0xC0) != 0x80) {
        ok = false;
      }
    }
    if (ok && n == 2) {
      unsigned char c1 = static_cast<unsigned char>(raw[i + 1]);
      if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) {
        ok = false;
      }
    }
    if (ok && n == 3) {
      unsigned char c1 = static_cast<unsigned char>(raw[i + 1]);
      if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) {
        ok = false;
      }
    }
    if (!ok) {
      bad(c);
      i++;
      continue;
    }
    if (c == 0) {
      i++;  // U+0000 stripped
      continue;
    }
    out.append(raw, i, static_cast<size_t>(n) + 1);
    i += static_cast<size_t>(n) + 1;
  }
  return out;
}

}  // namespace

std::optional<std::string> ton_transfer_comment(const td::Ref<vm::Cell> &body) {
  auto r_ctx = open_body(body);
  if (r_ctx.is_error()) {
    return std::nullopt;
  }
  auto ctx = r_ctx.move_as_ok();
  auto &cs = ctx.cs;
  if (cs.size() < 32) {
    return std::nullopt;  // TonTransferMessage: comment None
  }
  auto op = static_cast<td::uint32>(cs.fetch_ulong(32));
  bool encrypted = op == 0x2167da4b;
  if (!(cs.size() >= 8 && cs.size() % 8 == 0 &&
        (cs.size_refs() == 0 || cs.size_refs() == 1))) {
    return std::nullopt;
  }
  PSlice ps;
  ps.cs = cs;
  for (unsigned i = 0; i < cs.size_refs(); i++) {
    ps.refs.push_back(cs.prefetch_ref(i));
  }
  auto r = load_snake_bytes(ps);
  if (r.is_error()) {
    return std::nullopt;  // Python's inner try swallows
  }
  std::string bytes = r.move_as_ok();
  if (encrypted) {
    return td::base64_encode(td::Slice(bytes));
  }
  return decode_backslashreplace_strip_nul(bytes);
}

// --- --parse-dump mode -------------------------------------------------------
//
// DUMP FORMAT CONTRACT (byte-for-byte twin: scratchpad/py_msg_dump.py):
//   === <fixture-basename>
//   <msg_hash> <MsgType> {k=v,...}     # parse ok; keys sorted
//   <msg_hash> <MsgType> PARSE_FAIL    # any parse/BOC failure
// Messages deduped by msg_hash within a fixture, lines sorted bytewise.
// Value rendering: null | true/false | decimal int | wc:HEX account |
// str:<s> | b64:<base64 raw bytes> | cell:<HASH-hex-upper> | {k=v,...} dict
// with sorted keys | extern:<len>:<value> external address (as Str).

namespace {

struct DumpType {
  td::uint32 opcode;
  const char *name;
};

constexpr DumpType kDumpTypes[] = {
    {0x0f8a7ea5, "JettonTransfer"},
    {0x178d4519, "JettonInternalTransfer"},
    {0x595f07bc, "JettonBurn"},
    {0x7362d09c, "JettonNotify"},
    {0x642b7d07, "JettonMint"},
    {0x00000015, "MinterJettonMint"},
    {0x4eb1f0f9, "ChangeDnsRecordMessage"},
    {0xa7733acd, "VestingSendMessage"},
    {0x01f3835d, "PTonTransfer"},
    {0xad4eb6f5, "DedustPayoutFromPool"},
    {0x9c610de3, "DedustSwapNotification"},
    {0xc0ffee06, "CoffeeCreateVault"},
    {0x00000002, "EvaaWithdrawMaster"},
    {0x00000211, "EvaaWithdrawCollateralized"},
    {0x000021e6, "EvaaWithdrawFailExcess"},
    {0x000021e7, "EvaaWithdrawFailExcess"},
    {0x000021e8, "EvaaWithdrawFailExcess"},
    {0x000021ec, "EvaaWithdrawFailExcess"},
    {0xcaae25a1, "LayerZeroOappExecuteCallback"},
};

std::string render_value(const Value &v);

std::string render_fields_sorted(const Value::Fields &fields) {
  std::vector<std::pair<std::string, std::string>> kv;
  kv.reserve(fields.size());
  for (const auto &f : fields) {
    kv.emplace_back(f.first, render_value(f.second));
  }
  std::sort(kv.begin(), kv.end());
  std::string out = "{";
  for (size_t i = 0; i < kv.size(); i++) {
    if (i) {
      out += ",";
    }
    out += kv[i].first + "=" + kv[i].second;
  }
  out += "}";
  return out;
}

std::string render_value(const Value &v) {
  switch (v.t) {
    case VType::Null:
      return "null";
    case VType::Bool:
      return v.boolean ? "true" : "false";
    case VType::Int:
    case VType::Amount:
      return v.num->to_dec_string();
    case VType::Str:
      return "str:" + v.str;
    case VType::Bytes:
      // FULL raw bytes (B0/D4 verification): pytoniq and td produce
      // byte-identical BOC containers for the same cell tree and flag set,
      // so payload bytes render as their exact base64 and are compared raw.
      return "b64:" + td::base64_encode(td::Slice(v.str));
    case VType::Account:
      return v.addr_none ? "addr_none" : v.str;
    case VType::Cell: {
      auto h = v.cell->get_hash();
      return "cell:" + hex_upper(h.as_slice().ubegin(), h.as_slice().size());
    }
    case VType::Asset:
      return v.is_ton ? "asset:TON" : "asset:" + v.str;
    case VType::Dict:
    case VType::Obj:
      return render_fields_sorted(*v.fields);
    default:
      return v.describe();
  }
}

const char *dump_type_for_opcode(td::uint32 op) {
  for (const auto &t : kDumpTypes) {
    if (t.opcode == op) {
      return t.name;
    }
  }
  return nullptr;
}

void dump_fixture(const std::string &path) {
  auto r_trace = load_trace(path);
  if (r_trace.is_error()) {
    std::printf("ERROR: %s\n", r_trace.error().message().c_str());
    return;
  }
  auto trace = r_trace.move_as_ok();
  std::set<std::string> seen;
  std::vector<std::string> lines;
  for (const auto &tx : trace.transactions) {
    for (const auto &m : tx->messages) {
      auto op = m->opcode32();
      if (!op || !m->content || m->content->body.empty()) {
        continue;
      }
      const char *type_name = dump_type_for_opcode(*op);
      if (type_name == nullptr) {
        continue;
      }
      if (!seen.insert(m->msg_hash).second) {
        continue;
      }
      std::string rendered;
      auto r_raw = td::base64_decode(td::Slice(m->content->body));
      if (r_raw.is_error()) {
        rendered = "PARSE_FAIL";
      } else {
        auto r_roots = vm::std_boc_deserialize_multi(r_raw.ok());
        if (r_roots.is_error() || r_roots.ok().empty()) {
          rendered = "PARSE_FAIL";
        } else {
          auto r_val = parse_message_body(type_name, r_roots.ok()[0]);
          rendered = r_val.is_ok() ? render_value(r_val.ok()) : "PARSE_FAIL";
        }
      }
      lines.push_back(m->msg_hash + " " + type_name + " " + rendered);
    }
  }
  std::sort(lines.begin(), lines.end());
  for (const auto &l : lines) {
    std::printf("%s\n", l.c_str());
  }
}

}  // namespace

int run_parse_dump(const std::vector<std::string> &paths) {
  namespace fs = std::filesystem;
  std::vector<std::string> fixtures;
  for (const auto &p : paths) {
    if (fs::is_directory(p)) {
      std::vector<std::string> names;
      for (const auto &e : fs::directory_iterator(p)) {
        if (e.path().extension() == ".lz4") {
          names.push_back(e.path().string());
        }
      }
      std::sort(names.begin(), names.end(),
                [](const std::string &a, const std::string &b) {
                  return fs::path(a).filename().string() < fs::path(b).filename().string();
                });
      fixtures.insert(fixtures.end(), names.begin(), names.end());
    } else {
      fixtures.push_back(p);
    }
  }
  for (const auto &f : fixtures) {
    std::printf("=== %s\n", fs::path(f).filename().string().c_str());
    dump_fixture(f);
  }
  return 0;
}

}  // namespace mch
