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

// Serialized BOC bytes, mirroring pytoniq Cell.to_boc(hash_crc32=True).
td::Result<std::string> boc_bytes(const td::Ref<vm::Cell> &c) {
  TRY_RESULT(b, vm::std_boc_serialize(c, vm::BagOfCells::Mode::WithCRC32C));
  return b.as_slice().str();
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
    case VType::Bytes: {
      // BOC payload bytes (custom_payload / forward_payload / comment that
      // happens to be a BOC) render as the ROOT CELL hash: pytoniq and td
      // serialize the same cell tree with different ref layouts, so the raw
      // container bytes are NOT cross-language comparable while the content is.
      if (v.str.size() >= 4 && static_cast<unsigned char>(v.str[0]) == 0xb5 &&
          static_cast<unsigned char>(v.str[1]) == 0xee && static_cast<unsigned char>(v.str[2]) == 0x9c &&
          static_cast<unsigned char>(v.str[3]) == 0x72) {
        auto r = vm::std_boc_deserialize(td::Slice(v.str));
        if (r.is_ok()) {
          auto h = r.ok()->get_hash();
          return "boc:" + hex_upper(h.as_slice().ubegin(), h.as_slice().size());
        }
      }
      return "b64:" + td::base64_encode(td::Slice(v.str));
    }
    case VType::Account:
      return v.addr_none ? "addr_none" : v.str;
    case VType::Cell: {
      auto h = v.cell->get_hash();
      return "cell:" + hex_upper(h.as_slice().ubegin(), h.as_slice().size());
    }
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
