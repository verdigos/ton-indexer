// StateInit readers take listing parameters from the deployment message and
// select the layout by code hash. Parse failures return Null.
#include "host/HostImpls.h"

#include "host/HostCommon.h"

#include "BlockTree.h"
#include "BuildRuntime.h"
#include "HostRegistry.h"
#include "parse/PSlice.h"
#include "btypes_gen.h"

#include "td/utils/base64.h"
#include "vm/boc.h"
#include "vm/cellslice.h"

#include <map>
#include <string>
#include <vector>

namespace mch {

namespace {

// Unlisted or missing code hash falls back to "latest", which each reader
// resolves to its newest layout.
const std::map<std::string, std::string> &sale_versions() {
  static const std::map<std::string, std::string> m = {
      {"2pufziLofEllctIDZSWVebzO+RpyA1fMvowFLvyb4I8=", "v1"},
      {"gnj0xSM95vvtyWmvUZNEp6m//FRIVtuphqlcC8+Fcck=", "v2"},
      {"MgUN+sRPZIZrzIbyzZ4TBf6dyts5WcACI3z7CQLUQyM=", "v3"},
      {"3rU7bFdlwebNI4v0e8XoO6WWvcwEsLhM1Qqx5HSgjzE=", "v3r2"},
      {"JCIfpXHlQuBVx3vt/b9SfHr0YM/cfzRMRQeHtM+h600=", "v3r3"},
      {"a5WmQYucnSNZBF0edVm41UmuDlBvJMqrWPowyPsf64Y=", "v4r1"},
  };
  return m;
}

const std::map<std::string, std::string> &auction_versions() {
  static const std::map<std::string, std::string> m = {
      {"zlp4U06qps7tja/UhtB262CpsNbb+1Nnb2YmScBomVY=", "v4r1"},
      {"ZmiHL6eXBUQ//UdSPo6eqfdquZ+aC1nSfej4GhwnudQ=", "v2"},
      {"G9nFo5v/t6DzQViLXdkrgTqEK/Ze8UEJOCIAzq+Pct8=", "v3r2"},
      {"u29ireD+stefqzuK6/CTCvmFU99gCTsgJ/Covxab/Ow=", "v3r3"},
      {"/ACindAgW83MDT/7nKOMw8jBWexg2KpUMkCpLxBZLUA=", "v1"},
  };
  return m;
}

std::string pick_version(const std::map<std::string, std::string> &table, const Value &code_hash,
                         const char *latest) {
  if (code_hash.t == VType::Str) {
    auto it = table.find(code_hash.str);
    if (it != table.end()) {
      return it->second;
    }
  }
  return latest;
}

// StateInit data cell of a deploying message.
td::Result<vm::CellSlice> state_init_data(const Block *b) {
  const Message *msg = block_msg(b);
  if (msg == nullptr || !msg->init_state) {
    return td::Status::Error("no init_state");
  }
  TRY_RESULT(raw, td::base64_decode(td::Slice(msg->init_state->body)));
  TRY_RESULT(root, vm::std_boc_deserialize(raw));
  TRY_RESULT(cs, open_ref_cell(root));
  TRY_RESULT(data, state_init_data_cell(cs));
  return open_ref_cell(data);
}

// Fixed-width unsigned read; underflow is a parse reject.
td::Result<td::RefInt256> load_uint_py(vm::CellSlice &cs, int bits) {
  if (!cs.have(bits)) return td::Status::Error("uint underflow");
  return cs.fetch_int256(bits, false);
}

td::Status skip_bits(vm::CellSlice &cs, int bits) {
  if (!cs.have(bits)) return td::Status::Error("bits underflow");
  cs.advance(bits);
  return td::Status::OK();
}

td::Status skip_address(vm::CellSlice &cs) {
  TRY_RESULT(v, load_address_py(cs));
  (void)v;
  return td::Status::OK();
}

td::Status skip_coins(vm::CellSlice &cs) {
  TRY_RESULT(v, load_coins_py(cs));
  (void)v;
  return td::Status::OK();
}

Value opt_amount(const td::RefInt256 &v) {
  return v.is_null() ? Value::null() : Value::make_int(v);
}

td::Result<vm::CellSlice> load_ref_slice(vm::CellSlice &cs) {
  if (cs.size_refs() == 0) return td::Status::Error("ref missing");
  return open_ref_cell(cs.fetch_ref());
}


td::Result<Value> parse_sale_data(vm::CellSlice cs, const std::string &version) {
  Value marketplace_address{Value::null()};
  Value marketplace_fee_address{Value::null()};
  Value royalty_address{Value::null()};
  td::RefInt256 full_price;
  td::RefInt256 marketplace_fee;
  td::RefInt256 royalty_amount;

  if (version == "v4r1") {
    TRY_STATUS(skip_bits(cs, 1));  // is_complete
    TRY_RESULT_ASSIGN(marketplace_address, load_address_py(cs));
    TRY_STATUS(skip_address(cs));  // nft_owner_address
    TRY_RESULT_ASSIGN(full_price, load_coins_py(cs));
    TRY_STATUS(skip_bits(cs, 32));  // sold_at
    TRY_STATUS(skip_bits(cs, 64));  // query_id
    TRY_RESULT(stat, load_ref_slice(cs));
    TRY_RESULT_ASSIGN(marketplace_fee_address, load_address_py(stat));
    TRY_RESULT_ASSIGN(royalty_address, load_address_py(stat));
    // fee_percent / royalty_percent never reach the block; the reader returns
    // early, so marketplace_fee / royalty_amount stay null on this version.
    TRY_STATUS(skip_bits(stat, 17));
    TRY_STATUS(skip_bits(stat, 17));
  } else {
    if (version == "v2" || version == "v3" || version == "v3r2" || version == "v3r3") {
      TRY_STATUS(skip_bits(cs, 1));   // is_complete
      TRY_STATUS(skip_bits(cs, 32));  // created_at
    }
    TRY_RESULT_ASSIGN(marketplace_address, load_address_py(cs));
    TRY_STATUS(skip_address(cs));  // nft_address
    TRY_STATUS(skip_address(cs));  // nft_owner_address
    TRY_RESULT_ASSIGN(full_price, load_coins_py(cs));
    TRY_RESULT(fees, load_ref_slice(cs));
    if (version == "v1") {
      TRY_RESULT_ASSIGN(marketplace_fee, load_coins_py(fees));
      TRY_RESULT_ASSIGN(marketplace_fee_address, load_address_py(fees));
      TRY_RESULT_ASSIGN(royalty_address, load_address_py(fees));
      TRY_RESULT_ASSIGN(royalty_amount, load_coins_py(fees));
    } else {  // v2, v3, v3r2, v3r3 use the opposite address/amount order.
      TRY_RESULT_ASSIGN(marketplace_fee_address, load_address_py(fees));
      TRY_RESULT_ASSIGN(marketplace_fee, load_coins_py(fees));
      TRY_RESULT_ASSIGN(royalty_address, load_address_py(fees));
      TRY_RESULT_ASSIGN(royalty_amount, load_coins_py(fees));
    }
  }

  Value::Fields f;
  f.emplace_back("full_price", opt_amount(full_price));
  f.emplace_back("marketplace_address", marketplace_address);
  f.emplace_back("marketplace_fee_address", marketplace_fee_address);
  f.emplace_back("marketplace_fee", opt_amount(marketplace_fee));
  f.emplace_back("royalty_address", royalty_address);
  f.emplace_back("royalty_amount", opt_amount(royalty_amount));
  return Value::make_obj(std::move(f));
}


struct AuctionInit {
  Value mp_fee_addr{Value::null()};
  Value royalty_fee_addr{Value::null()};
  td::RefInt256 mp_fee_factor, mp_fee_base, royalty_fee_base, max_bid, min_bid;
};

td::Status parse_auction_v1(vm::CellSlice &cs, AuctionInit &d) {
  TRY_RESULT(fees, load_ref_slice(cs));
  TRY_RESULT(bids, load_ref_slice(cs));
  TRY_RESULT_ASSIGN(d.mp_fee_addr, load_address_py(fees));
  TRY_RESULT_ASSIGN(d.mp_fee_factor, load_uint_py(fees, 32));
  TRY_RESULT_ASSIGN(d.mp_fee_base, load_uint_py(fees, 32));
  TRY_RESULT_ASSIGN(d.royalty_fee_addr, load_address_py(fees));
  TRY_STATUS(skip_bits(fees, 32));  // royalty_fee_factor
  TRY_RESULT_ASSIGN(d.royalty_fee_base, load_uint_py(fees, 32));
  TRY_RESULT_ASSIGN(d.min_bid, load_coins_py(bids));
  TRY_RESULT_ASSIGN(d.max_bid, load_coins_py(bids));
  return td::Status::OK();
}

td::Status parse_auction_v3r2(vm::CellSlice &cs, AuctionInit &d) {
  TRY_STATUS(skip_bits(cs, 3));  // end?, activated?, is_canceled?
  TRY_STATUS(skip_address(cs));  // last_member
  TRY_STATUS(skip_coins(cs));    // last_bid
  TRY_STATUS(skip_bits(cs, 32));  // last_bid_at
  TRY_STATUS(skip_bits(cs, 32));  // end_time
  TRY_RESULT(fees, load_ref_slice(cs));
  TRY_RESULT(constant, load_ref_slice(cs));
  TRY_STATUS(skip_bits(constant, 32));  // sub_gas_price_from_bid
  TRY_STATUS(skip_address(constant));   // mp_addr
  TRY_RESULT_ASSIGN(d.min_bid, load_coins_py(constant));
  TRY_RESULT_ASSIGN(d.max_bid, load_coins_py(constant));
  TRY_RESULT_ASSIGN(d.mp_fee_addr, load_address_py(fees));
  TRY_RESULT_ASSIGN(d.mp_fee_factor, load_uint_py(fees, 32));
  TRY_RESULT_ASSIGN(d.mp_fee_base, load_uint_py(fees, 32));
  TRY_RESULT_ASSIGN(d.royalty_fee_addr, load_address_py(fees));
  TRY_STATUS(skip_bits(fees, 32));  // royalty_fee_factor
  TRY_RESULT_ASSIGN(d.royalty_fee_base, load_uint_py(fees, 32));
  return td::Status::OK();
}

td::Status parse_auction_v3r3(vm::CellSlice &cs, AuctionInit &d) {
  TRY_STATUS(skip_bits(cs, 2));  // end?, is_canceled?
  TRY_STATUS(skip_address(cs));  // last_member
  TRY_STATUS(skip_coins(cs));    // last_bid
  TRY_STATUS(skip_bits(cs, 32));  // last_bid_at
  TRY_STATUS(skip_bits(cs, 32));  // end_time
  TRY_STATUS(skip_address(cs));   // nft_owner
  TRY_STATUS(skip_bits(cs, 64));  // last_query_id
  TRY_RESULT_ASSIGN(d.mp_fee_factor, load_uint_py(cs, 32));
  TRY_RESULT_ASSIGN(d.mp_fee_base, load_uint_py(cs, 32));
  TRY_STATUS(skip_bits(cs, 32));  // royalty_fee_factor
  TRY_RESULT_ASSIGN(d.royalty_fee_base, load_uint_py(cs, 32));
  TRY_RESULT(fees, load_ref_slice(cs));
  TRY_RESULT(constant, load_ref_slice(cs));
  TRY_RESULT_ASSIGN(d.mp_fee_addr, load_address_py(fees));
  TRY_RESULT_ASSIGN(d.royalty_fee_addr, load_address_py(fees));
  TRY_STATUS(skip_address(constant));  // mp_addr
  TRY_RESULT_ASSIGN(d.min_bid, load_coins_py(constant));
  TRY_RESULT_ASSIGN(d.max_bid, load_coins_py(constant));
  return td::Status::OK();
}

td::Status parse_auction_v4r1(vm::CellSlice &cs, AuctionInit &d) {
  TRY_STATUS(skip_bits(cs, 2));  // end?, is_canceled?
  TRY_STATUS(skip_address(cs));  // last_member
  TRY_STATUS(skip_coins(cs));    // last_bid
  TRY_STATUS(skip_bits(cs, 32));  // last_bid_at
  TRY_STATUS(skip_bits(cs, 32));  // end_time
  TRY_STATUS(skip_address(cs));   // nft_owner
  TRY_STATUS(skip_bits(cs, 64));  // last_query_id
  TRY_RESULT(fees, load_ref_slice(cs));
  TRY_RESULT(constant, load_ref_slice(cs));
  TRY_RESULT_ASSIGN(d.mp_fee_addr, load_address_py(fees));
  TRY_RESULT_ASSIGN(d.royalty_fee_addr, load_address_py(fees));
  TRY_RESULT_ASSIGN(d.mp_fee_factor, load_uint_py(fees, 32));
  TRY_RESULT_ASSIGN(d.mp_fee_base, load_uint_py(fees, 32));
  TRY_STATUS(skip_bits(fees, 32));  // royalty_fee_factor
  TRY_RESULT_ASSIGN(d.royalty_fee_base, load_uint_py(fees, 32));
  TRY_STATUS(skip_address(constant));  // mp_addr
  TRY_RESULT_ASSIGN(d.min_bid, load_coins_py(constant));
  TRY_RESULT_ASSIGN(d.max_bid, load_coins_py(constant));
  return td::Status::OK();
}


bool has_deployment(const Block *b) {
  if (b == nullptr) {
    return false;
  }
  for (const EventNode *n : b->event_nodes) {
    const Transaction *tx = n != nullptr ? n->tx : nullptr;
    if (tx != nullptr && tx->orig_status != "active" && tx->end_status == "active") {
      return true;
    }
  }
  return false;
}

}  // namespace


// Existence of a deployment on the candidate. Btype is gated in the pattern.
// `> 0`, not `== 1`: deliberately weaker than nft_mint's single_contract_deploy.
bool sale_contract_deploy(const Block *b) {
  return has_deployment(b);
}

EvalResult getgems_sale_init(BuildEnv &env, const std::vector<Value> &args) {
  (void)env;
  const Block *b = as_block(args[0]);
  if (b == nullptr) {
    return rt_ok(Value::null());
  }
  auto r_data = state_init_data(b);
  if (r_data.is_error()) {
    return rt_ok(Value::null());  // StateInit parse failure is Null
  }
  auto r = parse_sale_data(r_data.move_as_ok(), pick_version(sale_versions(), args[1], "v4r1"));
  return rt_ok(r.is_ok() ? r.move_as_ok() : Value::null());
}

EvalResult getgems_auction_init(BuildEnv &env, const std::vector<Value> &args) {
  (void)env;
  const Block *b = as_block(args[0]);
  if (b == nullptr) {
    return rt_ok(Value::null());
  }
  auto r_data = state_init_data(b);
  if (r_data.is_error()) {
    return rt_ok(Value::null());
  }
  vm::CellSlice cs = r_data.move_as_ok();
  const std::string version = pick_version(auction_versions(), args[1], "v3r3");
  AuctionInit d;
  td::Status st = td::Status::Error("unsupported auction version");
  if (version == "v1") {
    st = parse_auction_v1(cs, d);
  } else if (version == "v2" || version == "v3r2") {
    // v2 and v3r2 have the same storage layout
    st = parse_auction_v3r2(cs, d);
  } else if (version == "v3r3") {
    st = parse_auction_v3r3(cs, d);
  } else if (version == "v4r1") {
    st = parse_auction_v4r1(cs, d);
  }
  if (st.is_error()) {
    return rt_ok(Value::null());
  }
  Value::Fields f;
  f.emplace_back("mp_fee_addr", d.mp_fee_addr);
  f.emplace_back("mp_fee_factor", opt_amount(d.mp_fee_factor));
  f.emplace_back("mp_fee_base", opt_amount(d.mp_fee_base));
  f.emplace_back("royalty_fee_addr", d.royalty_fee_addr);
  f.emplace_back("royalty_fee_base", opt_amount(d.royalty_fee_base));
  f.emplace_back("max_bid", opt_amount(d.max_bid));
  f.emplace_back("min_bid", opt_amount(d.min_bid));
  return rt_ok(Value::make_obj(std::move(f)));
}

// Finish/stop purchases place the seller payout beside the produced block.
// Insert a proxy under the sale parent, then absorb the peeked payout and proxy.
void getgems_proxy_insert(Block *produced, const ShaperMatch &m) {
  Block *payout = m.capture("sib_payout");
  if (produced == nullptr || payout == nullptr || m.arena == nullptr) {
    return;
  }
  Block *parent = produced->previous_block;
  if (parent == nullptr || parent->btype != mch::btype::kTonTransfer) {
    return;
  }
  const Value *comment = parent->data.field("comment");
  if (comment == nullptr || comment->t != VType::Str ||
      (comment->str != "finish" && comment->str != "stop")) {
    return;
  }
  Block *proxy = m.arena->make(mch::btype::kEmpty);
  parent->insert_between({produced, payout}, proxy);
  produced->merge_blocks({payout, proxy});
  produced->compact_connections();
}

}  // namespace mch
