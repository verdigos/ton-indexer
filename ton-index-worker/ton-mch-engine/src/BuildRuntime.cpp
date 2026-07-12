#include "BuildRuntime.h"

#include "BlockTree.h"
#include "MsgParse.h"
#include "parse/PSlice.h"

#include "td/utils/base64.h"
#include "vm/boc.h"
#include "vm/cellslice.h"

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

EvalResult rt_parse_expr(BuildEnv &env, const Value &target,
                         const std::vector<std::string> &types) {
  (void)env;  // stateless: no env.bodies store, unlike the statement form
  // expr.py _eval_parse: a null target or a non-block target (no get_body)
  // faults — the deliberate inversion of rt_parse's soft no-op on those.
  if (target.t != VType::Block || target.block == nullptr) {
    return rt_fault("parse expression: target is null or not a block");
  }
  Value body;
  EvalResult r = soft_parse(target.block, types, body);
  if (r.faulted) {
    return r;  // unregistered type name
  }
  if (body.is_null()) {
    // Every alternative failed to parse the body: total failure -> fault
    // (build reject), NOT the statement form's soft-null.
    return rt_fault("parse expression: no message type parsed the body");
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

// --- two-phase lookup runtime (B2) -------------------------------------------

std::string lookup_key(const std::string &kind, const std::vector<Value> &args) {
  // Type-tagged + length-prefixed encoding (§4.1). NUL-concatenation was
  // ambiguous — an embedded NUL in a payload, a differing arity, or the same
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
  // dedust_pool is fetched by the dedust_swap_legs host fn via the two-phase
  // table (not a `lookup` build-program node), so it is absent from any
  // matcher's ref_lookups and never affected build_skip_reason. It is listed
  // here so has()/runnability gating recognises it honestly; behaviour is
  // unchanged (no matcher references it as a lookup node, so no skip decision
  // moves on either the C++ or Python side).
  static const std::set<std::string> k = {"jetton_wallet", "nft_item", "nominator_pool",
                                          "jvault_assets", "dedust_pool", "nft_sale",
                                          "nft_auction"};
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
  if (kind == "nft_sale") {
    // TestInterfaceRepository.get_nft_sale: interfaces[addr]["NftSale"] (RAW key)
    // -> NftSale ORM. The nft host fn reads marketplace_address / nft_address /
    // full_price / nft_owner_address.
    auto it = interfaces_->find(addr);
    if (it == interfaces_->end()) {
      return Value::null();
    }
    const Value *sale = it->second.field("NftSale");
    if (sale == nullptr) {
      return Value::null();
    }
    return iface_obj(*sale, {"marketplace_address", "nft_address", "full_price",
                             "nft_owner_address"});
  }
  if (kind == "nft_auction") {
    // TestInterfaceRepository.get_nft_auction: interfaces[addr]["NftAuction"]
    // (RAW key) -> NftAuction ORM. The nft host fn reads mp_addr / nft_addr /
    // last_bid / nft_owner.
    auto it = interfaces_->find(addr);
    if (it == interfaces_->end()) {
      return Value::null();
    }
    const Value *auc = it->second.field("NftAuction");
    if (auc == nullptr) {
      return Value::null();
    }
    return iface_obj(*auc, {"mp_addr", "nft_addr", "last_bid", "nft_owner"});
  }
  if (kind == "nominator_pool") {
    auto it = interfaces_->find(addr);
    if (it == interfaces_->end() || it->second.field("NominatorPool") == nullptr) {
      return Value::null();
    }
    return it->second;  // the account's whole interfaces Dict
  }
  if (kind == "dedust_pool") {
    // TestInterfaceRepository.get_dedust_pool: interfaces[addr]["DedustPool"]
    // (RAW key == AccountId.as_str() upper) -> DedustPool(assets=data["assets"]).
    // Absent -> null; the host fn then rejects (Python None.assets raises).
    auto it = interfaces_->find(addr);
    if (it == interfaces_->end()) {
      return Value::null();
    }
    const Value *pool = it->second.field("DedustPool");
    if (pool == nullptr) {
      return Value::null();
    }
    return iface_obj(*pool, {"assets"});
  }
  if (kind == "jvault_assets") {
    // extract_jvault_assets (blocks/jvault.py) as a record lookup: read the
    // stake wallet's extra-data cell (b64 BOC string), parse two addresses
    // (staking_pool + minter), then chain a second extra-data read (pool ->
    // lock_wallet_address) and a jetton_wallet read (lock wallet -> jetton).
    // Python wraps the whole thing in try/except returning (None,None,None) and
    // the registered lookup maps that to Null; a MISSING pool extra-data (but
    // resolved staking_pool) is the partial case {staking_pool, null, jvault}.
    auto it = interfaces_->find(upper_ascii(addr));
    if (it == interfaces_->end()) {
      return Value::null();
    }
    const Value *extra = it->second.field("data_boc");
    if (extra == nullptr) {
      return Value::null();  // get_extra_data(stake_wallet) is None
    }
    const Value *boc_str = extra->field("data_boc");
    if (boc_str == nullptr || boc_str->t != VType::Str) {
      return Value::null();
    }
    auto r_raw = td::base64_decode(td::Slice(boc_str->str));
    if (r_raw.is_error()) {
      return Value::null();
    }
    auto r_cell = vm::std_boc_deserialize(r_raw.move_as_ok());
    if (r_cell.is_error() || r_cell.ok().is_null()) {
      return Value::null();
    }
    vm::CellSlice cs;
    bool special = false;
    try {
      cs = vm::load_cell_slice_special(r_cell.move_as_ok(), special);
    } catch (...) {
      return Value::null();
    }
    auto r_pool = load_address_py(cs);
    if (r_pool.is_error()) {
      return Value::null();
    }
    Value pool = r_pool.move_as_ok();
    auto r_minter = load_address_py(cs);
    if (r_minter.is_error()) {
      return Value::null();
    }
    Value minter = r_minter.move_as_ok();
    // Python then does staking_pool.to_str(...): addr_none (load_address None)
    // AttributeErrors into the outer except -> whole result None.
    if (pool.t != VType::Account || pool.addr_none) {
      return Value::null();
    }
    Value jvault_asset = Value::null();
    if (minter.t == VType::Account && !minter.addr_none) {
      auto mn = normalize_raw_address(minter.str);
      jvault_asset = Value::make_asset_jetton(mn ? *mn : minter.str);
    }
    auto make_record = [&](Value asset) {
      Value::Fields f;
      f.emplace_back("staking_pool", pool);
      f.emplace_back("asset", std::move(asset));
      f.emplace_back("jvault_asset", jvault_asset);
      return Value::make_obj(std::move(f));
    };
    auto it2 = interfaces_->find(upper_ascii(pool.str));
    const Value *pool_extra =
        it2 == interfaces_->end() ? nullptr : it2->second.field("data_boc");
    if (pool_extra == nullptr) {
      return make_record(Value::null());  // stake_pool_extra is None: partial
    }
    const Value *lock = pool_extra->field("lock_wallet_address");
    if (lock == nullptr || lock->t != VType::Str) {
      return Value::null();  // KeyError on ['lock_wallet_address'] -> except -> None
    }
    auto it3 = interfaces_->find(upper_ascii(lock->str));
    const Value *jw =
        it3 == interfaces_->end() ? nullptr : it3->second.field("JettonWallet");
    if (jw == nullptr) {
      return Value::null();  // get_jetton_wallet None -> .jetton AttributeError -> None
    }
    const Value *jetton = jw->field("jetton");
    if (jetton == nullptr || jetton->t != VType::Str) {
      return Value::null();
    }
    auto jn = normalize_raw_address(jetton->str);
    return make_record(Value::make_asset_jetton(jn ? *jn : jetton->str));
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
