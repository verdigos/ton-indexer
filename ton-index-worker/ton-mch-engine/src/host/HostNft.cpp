#include "host/HostImpls.h"

#include "host/HostCommon.h"

#include "BlockTree.h"
#include "BuildRuntime.h"
#include "HostRegistry.h"
#include "parse/PSlice.h"
#include "btypes_gen.h"

#include "vm/cellslice.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mch {

namespace {

constexpr std::uint32_t kTeleitemBidInfoOpcode = 0x38127DE1;

// Raw-address Str -> normalized Account; anything else (null, non-address)
// becomes addr_none.
Value account_from_value(const Value &v) {
  if (v.t == VType::Str) {
    return account_from_opt(std::optional<std::string>(v.str));
  }
  if (v.t == VType::Account) {
    return v;
  }
  return Value::make_account_none();
}


const EventNode *first_node(const Block *b) {
  return (b == nullptr || b->event_nodes.empty()) ? nullptr : b->event_nodes.front();
}


// Full NftOwnershipAssigned + NftPayload parse for the telegram core.
struct OwnershipFull {
  td::RefInt256 query_id;
  Value prev_owner{Value::null()};   // Account (empty -> addr_none)
  bool has_payload{false};
  std::string payload_raw_boc;       // payload cell as BOC with crc32
  bool is_bid{false};
  td::RefInt256 bid;
};

td::Result<OwnershipFull> parse_ownership_full(const td::Ref<vm::Cell> &body) {
  TRY_RESULT(ctx, open_body(body));
  auto &cs = ctx.cs;
  OwnershipFull out;
  if (!cs.have(32 + 64)) {
    return td::Status::Error("ownership: header underflow");
  }
  cs.advance(32);
  out.query_id = refint_u64(cs.fetch_ulong(64));
  TRY_RESULT_ASSIGN(out.prev_owner, load_address_value(cs));
  // Payload parse is soft: any failure leaves nft_payload unset.
  if (!cs.have(1)) {
    return out;  // no bit -> nft_payload unset
  }
  bool in_ref = cs.fetch_ulong(1) != 0;
  PSlice ps;
  if (in_ref) {
    if (cs.size_refs() == 0) {
      return out;  // missing ref -> nft_payload unset
    }
    ps = pslice_from_cell(cs.fetch_ref());
  } else {
    ps.cs = cs;
    ps.refs = ctx.all_refs;
    ps.off = 0;
  }
  // payload_raw_boc is the payload cell as BOC with crc32.
  auto r_cell = pslice_to_cell(ps);
  if (r_cell.is_error()) {
    return out;
  }
  auto r_boc = td_boc_serialize_crc(r_cell.move_as_ok());
  if (r_boc.is_error()) {
    return out;
  }
  out.payload_raw_boc = r_boc.move_as_ok();
  out.has_payload = true;
  // op peek on a copy; TeleitemBidInfo when op == 0x38127DE1.
  if (ps.cs.size() >= 32) {
    vm::CellSlice tmp = ps.cs;
    auto op = static_cast<std::uint32_t>(tmp.fetch_ulong(32));
    if (op == kTeleitemBidInfoOpcode) {
      auto r_bid = load_var_uint16(tmp);
      if (r_bid.is_ok()) {
        out.bid = r_bid.move_as_ok();
        out.is_bid = true;
      }
    }
  }
  return out;
}


// Returns the `nft` dict Value, or faults on a malformed content.
EvalResult get_nft_data(BuildEnv &env, const Value &nft_address) {
  const std::string &addr_str = nft_address.str;
  Value nft = env.lookups->get("nft_item", std::vector<Value>{Value::make_str(addr_str)});

  Value::Fields d;
  d.emplace_back("address", nft_address);
  Value index = Value::null();
  Value collection = Value::null();
  bool exists = false;
  Value name = Value::null();
  Value type = Value::null();
  Value meta = Value::null();
  bool has_name_type = false;
  bool has_meta = false;

  if (!nft.is_null()) {
    exists = true;
    const Value *idx = nft.field("index");
    index = idx != nullptr ? *idx : Value::null();
    const Value *content = nft.field("content");
    if (content == nullptr || content->t != VType::Dict) {
      // Non-dict content faults (the uri membership test would be undefined).
      return rt_fault("nft content is not a dict");
    }
    const Value *uri = content->field("uri");
    bool fragment = fragment_uri(uri);
    if (fragment) {
      // Last path segment minus 5 chars is name; previous segment is type.
      std::vector<std::string> toks;
      std::string cur;
      for (char c : uri->str) {
        if (c == '/') {
          toks.push_back(cur);
          cur.clear();
        } else {
          cur.push_back(c);
        }
      }
      toks.push_back(cur);
      std::string last = toks.back();
      std::string nm = last.size() >= 5 ? last.substr(0, last.size() - 5) : std::string{};
      std::string ty = toks.size() >= 2 ? toks[toks.size() - 2] : std::string{};
      name = Value::make_str(nm);
      type = Value::make_str(ty);
      has_name_type = true;
    } else {
      meta = *content;
      has_meta = true;
    }
    const Value *coll = nft.field("collection_address");
    if (coll != nullptr && !coll->is_null()) {
      Value::Fields cf;
      cf.emplace_back("address", account_from_value(*coll));
      collection = Value::make_dict(std::move(cf));
    }
  }

  d.emplace_back("index", std::move(index));
  d.emplace_back("collection", std::move(collection));
  d.emplace_back("exists", Value::make_bool(exists));
  if (has_name_type) {
    d.emplace_back("name", std::move(name));
    d.emplace_back("type", std::move(type));
  }
  if (has_meta) {
    d.emplace_back("meta", std::move(meta));
  }
  return rt_ok(Value::make_dict(std::move(d)));
}

}  // namespace

// nft_item metadata projection (get_nft_data + Fragment URI split). Null when
// the item is unknown so `require` rejects.
EvalResult nft_transfer_data(BuildEnv &env, const std::vector<Value> &args) {
  Value addr = args[0];
  if (addr.is_null()) {
    return rt_ok(Value::null());
  }
  if (addr.t == VType::Str) {
    addr = account_from_value(addr);
  } else if (addr.t != VType::Account) {
    return rt_fault("nft_transfer_data: address");
  }
  EvalResult r = get_nft_data(env, addr);
  if (r.faulted) {
    return r;
  }
  const Value *exists_f = r.value.field("exists");
  if (exists_f == nullptr || exists_f->t != VType::Bool || !exists_f->boolean) {
    return rt_ok(Value::null());
  }
  return r;
}

// Soft ownership+payload parse and shared metadata projection. Null for an
// unknown NFT item. Payout sort/select lives in the matcher.
EvalResult telegram_nft_purchase_data(BuildEnv &env, const std::vector<Value> &args) {
  const Block *assigned = as_block(args[0]);
  if (assigned == nullptr) {
    return rt_fault("telegram: null assigned");
  }
  const EventNode *anode = first_node(assigned);
  const Message *amsg = anode != nullptr ? anode->msg : nullptr;
  auto r_body = block_body(assigned);
  if (r_body.is_error()) {
    return rt_fault("telegram: assigned body");
  }
  auto r_own = parse_ownership_full(r_body.move_as_ok());
  if (r_own.is_error()) {
    return rt_fault("telegram: ownership parse");
  }
  OwnershipFull own = r_own.move_as_ok();

  Value new_owner = account_from_opt(amsg != nullptr ? amsg->destination : std::nullopt);
  Value prev_owner = own.prev_owner.addr_none ? Value::null() : own.prev_owner;

  Value nft_address = account_from_opt(amsg != nullptr ? amsg->source : std::nullopt);
  EvalResult r_nftd = get_nft_data(env, nft_address);
  if (r_nftd.faulted) {
    return r_nftd;
  }
  Value nft = std::move(r_nftd.value);
  const Value *exists_f = nft.field("exists");
  if (exists_f == nullptr || exists_f->t != VType::Bool || !exists_f->boolean) {
    return rt_ok(Value::null());
  }

  Value payload = Value::null();
  if (own.has_payload) {
    payload = Value::make_bytes(std::move(own.payload_raw_boc));
  }

  Value::Fields ns;
  ns.emplace_back("query_id", Value::make_int(std::move(own.query_id)));
  ns.emplace_back("prev_owner", std::move(prev_owner));
  ns.emplace_back("new_owner", std::move(new_owner));
  ns.emplace_back("nft", std::move(nft));
  ns.emplace_back("payload", std::move(payload));
  ns.emplace_back("is_bid", Value::make_bool(own.is_bid));
  ns.emplace_back("bid", own.is_bid ? Value::make_int(std::move(own.bid)) : Value::null());
  return rt_ok(Value::make_obj(std::move(ns)));
}

// Purchase branch only: absorb the funding parent (produced.previous_block)
// unless it is a finish/stop sale-contract ton_transfer.
void nft_transfer_parent_absorb(Block *produced, const ShaperMatch &) {
  if (produced->data.is_null()) {
    return;
  }
  const Value *ip = produced->data.field("is_purchase");
  if (ip == nullptr || ip->t != VType::Bool || !ip->boolean) {
    return;
  }
  Block *prev = produced->previous_block;
  if (prev == nullptr) {
    return;
  }
  Block *parent = nullptr;
  if (prev->btype == mch::btype::kTonTransfer) {
    const Value *c = prev->data.field("comment");
    std::string cs = (c != nullptr && c->t == VType::Str) ? c->str : std::string{};
    bool has = c != nullptr && c->t == VType::Str;
    if (!(has && (cs == "finish" || cs == "stop"))) {
      parent = prev;
    }
  } else if (prev->btype == mch::btype::kCallContract) {
    const Value *src = prev->data.field("source");
    if (src == nullptr || src->is_null()) {
      parent = prev;
    }
  }
  if (parent != nullptr) {
    produced->merge_blocks(std::vector<Block *>{parent});
    produced->compact_connections();
  }
}

// True when the block's event nodes deploy exactly one distinct account.
// Repeated deployment nodes for the same normalized account still count once.
// Frozen-to-active transactions qualify here even though they do not create the
// separate contract_deploy side-effect block.
bool single_contract_deploy(const Block *b) {
  if (b == nullptr) {
    return false;
  }
  std::set<std::string> deployed;
  for (const EventNode *n : b->event_nodes) {
    const Transaction *tx = n != nullptr ? n->tx : nullptr;
    if (tx == nullptr || tx->orig_status == "active" || tx->end_status != "active") {
      continue;
    }
    deployed.insert(tx->account);
  }
  return deployed.size() == 1;
}

}  // namespace mch
