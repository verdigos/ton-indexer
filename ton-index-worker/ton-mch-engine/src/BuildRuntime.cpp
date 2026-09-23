#include "BuildRuntime.h"
#include "LookupKinds.h"

#include "BlockTree.h"
#include "MsgParse.h"
#include "parse/PSlice.h"

#include "td/utils/base64.h"
#include "td/utils/logging.h"
#include "vm/boc.h"
#include "vm/cellslice.h"

#include <cstdio>

namespace mch {

namespace {

// First event node's message body cell. Any failure (no node / no content /
// undecodable BOC) fails that soft-parse alternative.
td::Result<td::Ref<vm::Cell>> block_body_cell(const Block *b) {
  if (b->event_nodes.empty() || b->event_nodes.front()->msg == nullptr ||
      !b->event_nodes.front()->msg->content) {
    return td::Status::Error("block has no message body");
  }
  TRY_RESULT(raw, td::base64_decode(td::Slice(b->event_nodes.front()->msg->content->body)));
  return vm::std_boc_deserialize(raw);
}

// First type whose parser succeeds wins; parser failures try the next
// alternative. A null body means target resolution failed.
EvalResult soft_parse(const td::Ref<vm::Cell> &body,
                      const std::vector<std::string> &types, Value &out) {
  for (const std::string &name : types) {
    if (message_parsers().find(name) == message_parsers().end()) {
      return rt_fault("message type '" + name + "' is not registered");
    }
    if (body.is_null()) {
      continue;
    }
    auto r = parse_message_body(name, body);
    if (r.is_ok()) {
      out = r.move_as_ok();
      return rt_ok(Value::null());
    }
  }
  out = Value::null();
  return rt_ok(Value::null());
}

EvalResult soft_parse(const Block *b, const std::vector<std::string> &types, Value &out) {
  auto r_body = block_body_cell(b);
  if (r_body.is_error()) {
    return soft_parse(td::Ref<vm::Cell>{}, types, out);
  }
  return soft_parse(r_body.move_as_ok(), types, out);
}

EvalResult parse_block(BuildEnv &env, const Block *block,
                       const std::vector<std::string> &types) {
  if (env.body_cache != nullptr) {
    auto it = env.body_cache->find(block);
    if (it != env.body_cache->end() && it->second.first == types) {
      env.bodies[block] = it->second.second;
      return rt_ok(Value::null());
    }
  }
  Value body;
  EvalResult r = soft_parse(block, types, body);
  if (r.faulted) {
    return r;
  }
  env.bodies[block] = body;
  if (env.body_cache != nullptr) {
    env.body_cache->insert_or_assign(block, BodyCache::mapped_type{types, std::move(body)});
  }
  return rt_ok(Value::null());
}

td::Result<td::Ref<vm::Cell>> parse_expr_cell(const Value &target) {
  if (target.t == VType::Block && target.block != nullptr) {
    return block_body_cell(target.block);
  }
  if (target.t == VType::Cell && target.cell.not_null()) {
    return target.cell;
  }
  if (target.t == VType::Str) {
    TRY_RESULT(raw, td::base64_decode(td::Slice(target.str)));
    return vm::std_boc_deserialize(raw);
  }
  return td::Status::Error("target is null or not a block, cell, or string");
}

}  // namespace

EvalResult rt_parse(BuildEnv &env, const Value &target, const std::vector<std::string> &types) {
  if (target.t == VType::List) {
    for (const Value &el : *target.items) {
      if (el.t != VType::Block || el.block == nullptr) {
        continue;  // null (gap) elements stay unparsed; non-blocks unreachable
      }
      EvalResult r = parse_block(env, el.block, types);
      if (r.faulted) {
        return r;
      }
    }
    return rt_ok(Value::null());
  }
  if (target.t == VType::Block && target.block != nullptr) {
    return parse_block(env, target.block, types);
  }
  // Null and other non-block targets leave the body unset.
  return rt_ok(Value::null());
}

EvalResult rt_parse_expr(const Value &target,
                         const std::vector<std::string> &types, bool nullable) {
  auto r_cell = parse_expr_cell(target);
  if (r_cell.is_error()) {
    return nullable ? rt_ok(Value::null())
                    : rt_fault("parse expression: " + r_cell.error().message().str());
  }
  Value body;
  EvalResult r = soft_parse(r_cell.move_as_ok(), types, body);
  if (r.faulted) {
    return r;  // unregistered type name
  }
  if (body.is_null()) {
    return nullable ? rt_ok(Value::null())
                    : rt_fault("parse expression: no message type parsed the body");
  }
  return rt_ok(std::move(body));
}

EvalResult rt_access_build(const BuildEnv &env, const Value &obj, const std::string &field) {
  if (field == "body") {
    if (obj.t == VType::Block) {
      auto it = env.bodies.find(obj.block);
      if (it == env.bodies.end()) {
        return rt_fault("`.body` accessed on a capture that was not `parse`d");
      }
      return rt_ok(it->second);
    }
    if (obj.t == VType::List) {
      std::vector<Value> out;
      for (const Value &el : *obj.items) {
        if (el.is_null()) {
          out.push_back(Value::null());
          continue;
        }
        if (el.t != VType::Block) {
          return rt_fault("`.body` accessed on a list capture that was not `parse`d");
        }
        auto it = env.bodies.find(el.block);
        if (it == env.bodies.end()) {
          return rt_fault("`.body` accessed on a list capture that was not `parse`d");
        }
        out.push_back(it->second);
      }
      return rt_ok(Value::make_list(std::move(out)));
    }
  }
  return rt_access(obj, field);
}


std::string lookup_key(const std::string &kind, const std::vector<Value> &args) {
  // Type-tagged, length-prefixed encoding. An embedded NUL, a differing arity, or the same
  // text carried by two different Value types could all collapse to the same
  // key and cross-wire a fetched lookup result. Here every segment is framed
  // as `<tag><decimal-len>:<payload>`, so no payload byte (NUL included) can
  // be mistaken for a separator and no two structurally distinct requests
  // share a key. The kind and the arg count are framed too.
  auto frame = [](std::string &out, char tag, const std::string &payload) {
    out += tag;
    out += std::to_string(payload.size());
    out += ':';
    out += payload;
  };
  std::string key;
  frame(key, 'K', kind);
  key += 'n';
  key += std::to_string(args.size());
  key += ';';
  for (const Value &a : args) {
    switch (a.t) {
      case VType::Str:
        frame(key, 's', a.str);
        break;
      case VType::Bytes:
        frame(key, 'b', a.str);
        break;
      case VType::Account:
        if (a.addr_none) {
          key += "a-;";  // addr_none: a distinct, payload-free tag
        } else {
          frame(key, 'a', a.str);
        }
        break;
      case VType::Int:
        frame(key, 'i', a.num.is_null() ? "nan" : a.num->to_dec_string());
        break;
      case VType::Amount:
        frame(key, 'm', a.num.is_null() ? "nan" : a.num->to_dec_string());
        break;
      default:
        frame(key, '?', a.describe());
        break;
    }
  }
  return key;
}

Value CollectingLookupTable::get(const std::string &kind, const std::vector<Value> &args) const {
  const std::string key = lookup_key(kind, args);
  auto it = fetched_.find(key);
  if (it != fetched_.end()) {
    return it->second;
  }
  misses.emplace(key, std::make_pair(kind, args));
  return Value::null();
}

namespace {

}  // namespace

const std::set<std::string> &lookup_kinds() {
  static const std::set<std::string> kinds = [] {
    std::set<std::string> result;
    for (const auto &kind : kLookupKindTable) result.emplace(kind.name);
    return result;
  }();
  return kinds;
}

EvalResult rt_lookup_build(const BuildEnv &env, const std::string &kind,
                           const std::vector<Value> &args) {
  // Check registration before the null-strict short-circuit. The evaluator
  // _eval_lookup resolves the kind first, so `lookup unknown(null)` faults.
  if (env.lookups == nullptr || !env.lookups->has(kind)) {
    return rt_fault("lookup kind '" + kind + "' is not registered");
  }
  for (const Value &a : args) {
    if (a.is_null()) {
      return rt_ok(Value::null());  // null-strict
    }
  }
  return rt_ok(env.lookups->get(kind, args));
}

// Host-reject observability

RejectCtx &reject_ctx() {
  static thread_local RejectCtx ctx;
  return ctx;
}

void reject_log(const std::string &reason) {
  const RejectCtx &c = reject_ctx();
  if (!c.final_pass) {
    return;  // a dry collect pass: rejects there are the empty table, not the data
  }
  const Transaction *tx = nullptr;
  std::string anchor = "?";
  if (c.anchor != nullptr) {
    anchor = c.anchor->btype;
    if (c.anchor->opcode) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "/0x%08x", *c.anchor->opcode);
      anchor += buf;
    }
    if (!c.anchor->event_nodes.empty()) {
      tx = c.anchor->event_nodes.front()->tx;
    }
  }
  LOG(WARNING) << "[mch-reject] trace=" << (c.trace_id != nullptr ? *c.trace_id : "?")
               << " matcher=" << (c.matcher != nullptr ? *c.matcher : "?")
               << " fn=" << (c.fn != nullptr ? *c.fn : "?") << " anchor=" << anchor
               << " tx=" << (tx != nullptr ? tx->hash : "?")
               << " account=" << (tx != nullptr ? tx->account : "?") << " reason=" << reason;
}

EvalResult host_reject(const std::string &reason) {
  reject_log(reason);
  return rt_ok(Value::null());
}

}  // namespace mch
