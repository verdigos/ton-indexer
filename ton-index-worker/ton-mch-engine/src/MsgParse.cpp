// TL-B message-parsing registry and dispatch (see MsgParse.h for the semantics
// contract). The per-family adapters live in src/parse/*; this file owns the
// message_parsers() map, parse_message_body dispatch, and the --parse-dump mode.
//
// FILE-LEVEL PYTONIQ-PARITY CATALOGUE (referenced by the split parser TUs):
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
#include "parse/PSlice.h"
#include "parse/Parsers.h"

#include "common/refint.h"
#include "td/utils/base64.h"
#include "vm/boc.h"
#include "vm/cellslice.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mch {

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
