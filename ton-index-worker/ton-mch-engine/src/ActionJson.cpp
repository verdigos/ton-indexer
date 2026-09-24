#include "ActionJson.h"

#include "ActionPayload.h"
#include "IndexData.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/base64.h"
#include "td/utils/misc.h"

#include <set>

namespace mch {
namespace {

std::string value_json(const Value &v) {
  td::JsonBuilder jb;
  {
    auto out = jb.enter_value();
    switch (v.t) {
      case VType::Null:
      case VType::Block:
        out << td::JsonNull();
        break;
      case VType::Bool:
        out << td::JsonBool(v.boolean);
        break;
      case VType::Int:
      case VType::Amount:
        if (v.num.is_null()) out << td::JsonNull();
        else out << v.num->to_dec_string();
        break;
      case VType::Str:
        if (v.str.find('\0') == std::string::npos) out << v.str;
        else {
          auto text = v.str;
          std::erase(text, '\0');
          out << text;
        }
        break;
      case VType::Account:
        if (v.addr_none) out << td::JsonNull();
        else out << canonicalize_or_passthrough(v.str);
        break;
      case VType::Asset:
        if (v.is_ton || !v.has_jetton) out << td::JsonNull();
        else out << canonicalize_or_passthrough(v.str);
        break;
      case VType::Bytes:
        out << td::base64_encode(td::Slice(v.str));
        break;
      case VType::Cell: {
        auto boc = td_boc_serialize_crc(v.cell);
        if (boc.is_error()) out << td::JsonNull();
        else out << td::base64_encode(td::Slice(boc.ok()));
        break;
      }
      case VType::List: {
        auto array = out.enter_array();
        for (const auto &item : *v.items) array << td::JsonRaw(value_json(item));
        break;
      }
      case VType::Dict:
      case VType::Obj: {
        auto object = out.enter_object();
        for (const auto &[key, val] : *v.fields) object(key, td::JsonRaw(value_json(val)));
        break;
      }
    }
  }
  return jb.string_builder().as_cslice().str();
}

std::string action_row_json_impl(const Action &a, const ActionRowContext &ctx, bool legacy) {
  td::JsonBuilder jb;
  {
    auto row = jb.enter_object();
    row("trace_id", ctx.trace_id)("action_id", a.action_id);
    row("start_lt", std::to_string(a.start_lt))("end_lt", std::to_string(a.end_lt));
    row("start_utime", std::to_string(a.start_utime))("end_utime", std::to_string(a.end_utime));
    row("mc_seqno_end", std::to_string(a.mc_seqno_end));
    row("trace_end_lt", std::to_string(ctx.trace_end_lt));
    row("trace_end_utime", std::to_string(ctx.trace_end_utime));
    row("trace_mc_seqno_end", std::to_string(ctx.trace_mc_seqno_end));
    if (ctx.trace_external_hash) row("trace_external_hash", *ctx.trace_external_hash);
    else row("trace_external_hash", td::JsonNull());
    if (ctx.trace_external_hash_norm) row("trace_external_hash_norm", *ctx.trace_external_hash_norm);
    else row("trace_external_hash_norm", td::JsonNull());
    row("type", a.type)("success", td::JsonBool(a.success));
    row("tx_hashes", td::json_array([&](auto &array) {
      for (const auto &hash : a.tx_hashes) array << hash;
    }));
    if (a.parent_action_id.empty()) row("parent_action_id", td::JsonNull());
    else row("parent_action_id", a.parent_action_id);
    row("ancestor_type", td::json_array([&](auto &array) {
      for (const auto &type : a.ancestor_type) array << type;
    }));
    row("value_extra_currencies", td::json_object([&](auto &object) {
      for (const auto &[id, amount] : a.value_extra_currencies) object(std::to_string(id), amount);
    }));
#define MCH_JSON_ENVELOPE(name) if (!omitted(a.name)) row(#name, td::JsonRaw(value_json(a.name)));
    MCH_JSON_ENVELOPE(source)
    MCH_JSON_ENVELOPE(source_secondary)
    MCH_JSON_ENVELOPE(destination)
    MCH_JSON_ENVELOPE(destination_secondary)
    MCH_JSON_ENVELOPE(asset)
    MCH_JSON_ENVELOPE(asset_secondary)
    MCH_JSON_ENVELOPE(asset2)
    MCH_JSON_ENVELOPE(asset2_secondary)
    MCH_JSON_ENVELOPE(amount)
    MCH_JSON_ENVELOPE(value)
    MCH_JSON_ENVELOPE(opcode)
#undef MCH_JSON_ENVELOPE
    if (legacy) {
      row("data", td::JsonRaw(omitted(a.data) ? "{}" : value_json(a.data)));
    } else {
      row("payload", "\\x" + td::hex_encode(action_payload_bytes(a)));
    }
  }
  return jb.string_builder().as_cslice().str();
}

}  // namespace

bool omitted(const Value &value) {
  return value.t == VType::Null || (value.t == VType::Amount && value.num.is_null());
}

ActionRowContext row_context(const schema::Trace &trace) {
  return {td::base64_encode(trace.trace_id.as_slice()),
          trace.external_hash ? std::make_optional(td::base64_encode(trace.external_hash->as_slice()))
                              : std::nullopt,
          trace.external_hash_norm ? std::make_optional(td::base64_encode(trace.external_hash_norm->as_slice()))
                                   : std::nullopt,
          static_cast<std::int64_t>(trace.end_lt), static_cast<std::int32_t>(trace.end_utime),
          trace.mc_seqno_end};
}

std::string action_row_json(const Action &a, const ActionRowContext &ctx) {
  return action_row_json_impl(a, ctx, true);
}

std::string action_legacy_details_json(const Action &a) {
  return omitted(a.data) ? "{}" : value_json(a.data);
}

std::string action_account_rows_json(const Action &a, const ActionRowContext &ctx) {
  std::set<std::string> accounts;
  for (const auto &account : a.accounts) accounts.insert(canonicalize_or_passthrough(account));
  td::JsonBuilder jb;
  {
    auto array = jb.enter_array();
    for (const auto &account : accounts) {
      array << td::json_object([&](auto &row) {
        row("action_id", a.action_id)("trace_id", ctx.trace_id)("account", account);
        row("trace_end_lt", std::to_string(ctx.trace_end_lt));
        row("action_end_lt", std::to_string(a.end_lt));
        row("trace_end_utime", std::to_string(ctx.trace_end_utime));
        row("trace_mc_seqno_end", std::to_string(ctx.trace_mc_seqno_end));
        row("action_end_utime", std::to_string(a.end_utime));
      });
    }
  }
  return jb.string_builder().as_cslice().str();
}

std::string action_rows_json(const std::vector<Action> &rows, const ActionRowContext &ctx) {
  td::JsonBuilder jb;
  {
    auto array = jb.enter_array();
    for (const auto &row : rows) array << td::JsonRaw(action_row_json_impl(row, ctx, false));
  }
  return jb.string_builder().as_cslice().str();
}

}  // namespace mch
