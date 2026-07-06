#include "BuildRuntime.h"

#include "BlockTree.h"
#include "MsgParse.h"

#include "td/utils/base64.h"
#include "vm/boc.h"

namespace mch {

namespace {

// Block.get_body(): the first event node's message body cell. Any failure
// (no node / no content / undecodable BOC) behaves like Python's get_body()
// raising INSIDE the per-type try of _soft_parse: that alternative fails.
td::Result<td::Ref<vm::Cell>> block_body_cell(const Block *b) {
  if (b->event_nodes.empty() || b->event_nodes.front()->msg == nullptr ||
      !b->event_nodes.front()->msg->content) {
    return td::Status::Error("block has no message body");
  }
  TRY_RESULT(raw, td::base64_decode(td::Slice(b->event_nodes.front()->msg->content->body)));
  return vm::std_boc_deserialize(raw);
}

// program.py _soft_parse: first type whose parser succeeds wins; a parser
// failure tries the next alternative; all failed -> Null body. An
// unregistered type name is an EvalError (fault -> rejection).
EvalResult soft_parse(const Block *b, const std::vector<std::string> &types, Value &out) {
  auto r_body = block_body_cell(b);
  for (const std::string &name : types) {
    if (message_parsers().find(name) == message_parsers().end()) {
      return rt_fault("message type '" + name + "' is not registered");
    }
    if (r_body.is_error()) {
      continue;  // get_body() raised inside this alternative's try
    }
    auto r = parse_message_body(name, r_body.ok());
    if (r.is_ok()) {
      out = r.move_as_ok();
      return rt_ok(Value::null());
    }
  }
  out = Value::null();
  return rt_ok(Value::null());
}

}  // namespace

EvalResult rt_parse(BuildEnv &env, const Value &target, const std::vector<std::string> &types) {
  if (target.is_null()) {
    return rt_ok(Value::null());  // absent optional capture: `.body` null-propagates
  }
  if (target.t == VType::List) {
    for (const Value &el : *target.items) {
      if (el.t != VType::Block || el.block == nullptr) {
        continue;  // null (gap) elements stay unparsed; non-blocks unreachable
      }
      Value body;
      EvalResult r = soft_parse(el.block, types, body);
      if (r.faulted) {
        return r;
      }
      env.bodies[el.block] = std::move(body);
    }
    return rt_ok(Value::null());
  }
  if (target.t == VType::Block && target.block != nullptr) {
    Value body;
    EvalResult r = soft_parse(target.block, types, body);
    if (r.faulted) {
      return r;
    }
    env.bodies[target.block] = std::move(body);
    return rt_ok(Value::null());
  }
  // Non-block target: Python's per-type try swallows the get_body attribute
  // error, leaving the body unset for anything `.body` could reach — no-op.
  return rt_ok(Value::null());
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

// --- two-phase lookup runtime (B2) -------------------------------------------

std::string lookup_key(const std::string &kind, const std::vector<Value> &args) {
  std::string key = kind;
  for (const Value &a : args) {
    key += static_cast<char>(0);
    switch (a.t) {
      case VType::Str:
      case VType::Bytes:
        key += a.str;
        break;
      case VType::Account:
        key += a.addr_none ? "addr_none" : a.str;
        break;
      case VType::Int:
      case VType::Amount:
        key += a.num.is_null() ? "nan" : a.num->to_dec_string();
        break;
      default:
        key += a.describe();
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

std::string upper_ascii(std::string s) {
  for (char &c : s) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return s;
}

// Project an interface fields Dict into an Obj with the deserializer's field
// set (TestInterfaceRepository -> ORM attribute access; a field absent from
// the fixture map becomes Null).
Value iface_obj(const Value &fields_dict, std::initializer_list<const char *> names) {
  Value::Fields fs;
  for (const char *n : names) {
    const Value *f = fields_dict.field(n);
    fs.emplace_back(n, f != nullptr ? *f : Value::null());
  }
  return Value::make_obj(std::move(fs));
}

}  // namespace

const std::set<std::string> &FixtureLookupSource::kinds() {
  static const std::set<std::string> k = {"jetton_wallet", "nft_item", "nominator_pool"};
  return k;
}

Value FixtureLookupSource::fetch(const std::string &kind, const std::vector<Value> &args) const {
  if (interfaces_ == nullptr || args.size() != 1 || args[0].t != VType::Str) {
    return Value::null();  // see the non-Str deviation note in the header
  }
  const std::string &addr = args[0].str;
  if (kind == "jetton_wallet") {
    auto it = interfaces_->find(upper_ascii(addr));
    if (it == interfaces_->end()) {
      return Value::null();
    }
    const Value *jw = it->second.field("JettonWallet");
    if (jw == nullptr) {
      return Value::null();
    }
    return iface_obj(*jw, {"balance", "address", "owner", "jetton"});
  }
  if (kind == "nft_item") {
    auto it = interfaces_->find(addr);  // RAW key (dns.py variant, no .upper())
    if (it == interfaces_->end()) {
      return Value::null();
    }
    const Value *nft = it->second.field("NftItem");
    if (nft == nullptr) {
      return Value::null();
    }
    return iface_obj(*nft, {"address", "init", "index", "collection_address",
                            "owner_address", "content"});
  }
  if (kind == "nominator_pool") {
    auto it = interfaces_->find(addr);
    if (it == interfaces_->end() || it->second.field("NominatorPool") == nullptr) {
      return Value::null();
    }
    return it->second;  // the account's whole interfaces Dict
  }
  return Value::null();
}

EvalResult rt_lookup_build(const BuildEnv &env, const std::string &kind,
                           const std::vector<Value> &args) {
  // Registration check BEFORE the null-strict short-circuit — expr.py
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

}  // namespace mch
