#include "EmuCapture.h"

#include "convert-utils.h"

#include "td/utils/JsonBuilder.h"
#include "td/utils/base64.h"
#include "td/utils/filesystem.h"

#include <functional>
#include <variant>

namespace mch {

namespace {

const char *finality_name(EmuFinality f) {
  switch (f) {
    case EmuFinality::emulated: return "emulated";
    case EmuFinality::confirmed: return "confirmed";
    case EmuFinality::finalized: return "finalized";
  }
  return "unknown";
}

// The trace_id is standard base64 (the Redis key); '+' and '/' are unusable in
// a file name.
std::string urlsafe(const std::string &b64) {
  std::string out;
  out.reserve(b64.size());
  for (char c : b64) {
    if (c == '+') {
      out += '-';
    } else if (c == '/') {
      out += '_';
    } else if (c != '=') {
      out += c;
    }
  }
  return out;
}

// td's JsonObjectScope can only take a *value* per field, so a nested object or
// array has to arrive as a Jsonable. One generic adapter beats a wrapper class
// per nesting level.
class JsonFn : public td::Jsonable {
 public:
  explicit JsonFn(std::function<void(td::JsonValueScope *)> fn) : fn_(std::move(fn)) {
  }
  void store(td::JsonValueScope *scope) const {
    fn_(scope);
  }

 private:
  std::function<void(td::JsonValueScope *)> fn_;
};

void put_int(td::JsonObjectScope &o, td::Slice key, const td::RefInt256 &v) {
  if (v.is_null()) {
    o(key, td::JsonNull());
  } else {
    o(key, v->to_dec_string());
  }
}

void put_addr(td::JsonObjectScope &o, td::Slice key, const block::StdAddress &a) {
  o(key, convert::to_raw_address(a));
}

void put_addr(td::JsonObjectScope &o, td::Slice key, const std::optional<block::StdAddress> &a) {
  if (a) {
    o(key, convert::to_raw_address(*a));
  } else {
    o(key, td::JsonNull());
  }
}

void store_nodes(td::JsonValueScope *scope, const EmuTraceSnapshot &snap) {
  auto arr = scope->enter_array();
  for (const EmuTxRec &n : snap.nodes) {
    auto value = arr.enter_value();
    auto o = value.enter_object();
    o("account", convert::to_raw_address(n.address));
    o("mc_seqno", static_cast<td::int64>(n.mc_seqno));
    o("finality", td::Slice(finality_name(n.finality)));
    // The snapshot already carries the BOC bytes convert::to_bytes would have
    // produced (EmuShadow.cpp), so this is only the base64 half of it.
    if (n.tx_boc.empty()) {
      o("tx_boc", td::JsonNull());
    } else {
      o("tx_boc", td::base64_encode(n.tx_boc.as_slice()));
    }
  }
}

// The tier-1 field shapes ParsedBlockLookupSource serves, so a replay of the
// capture resolves the same lookups the live run did.
void store_one_interface(td::JsonObjectScope &per_addr,
                         const schema::BlockchainInterfaceV2 &iface) {
  if (const auto *jw = std::get_if<schema::JettonWalletDataV2>(&iface)) {
    per_addr("jetton_wallet", JsonFn([jw](td::JsonValueScope *s) {
               auto o = s->enter_object();
               put_int(o, "balance", jw->balance);
               put_addr(o, "address", jw->address);
               put_addr(o, "owner", jw->owner);
               put_addr(o, "jetton", jw->jetton);
             }));
  } else if (const auto *nft = std::get_if<schema::NFTItemDataV2>(&iface)) {
    per_addr("nft_item", JsonFn([nft](td::JsonValueScope *s) {
               auto o = s->enter_object();
               put_addr(o, "address", nft->address);
               o("init", td::JsonBool(nft->init));
               put_int(o, "index", nft->index);
               put_addr(o, "collection_address", nft->collection_address);
               put_addr(o, "owner_address", nft->owner_address);
             }));
  } else if (const auto *s1 = std::get_if<schema::GetGemsNftFixPriceSaleData>(&iface)) {
    per_addr("nft_sale", JsonFn([s1](td::JsonValueScope *s) {
               auto o = s->enter_object();
               put_addr(o, "marketplace_address", s1->marketplace_address);
               put_addr(o, "nft_address", s1->nft_address);
               put_int(o, "full_price", s1->full_price);
               put_addr(o, "nft_owner_address", s1->nft_owner_address);
             }));
  } else if (const auto *a = std::get_if<schema::GetGemsNftAuctionData>(&iface)) {
    per_addr("nft_auction", JsonFn([a](td::JsonValueScope *s) {
               auto o = s->enter_object();
               put_addr(o, "mp_addr", a->mp_addr);
               put_addr(o, "nft_addr", a->nft_addr);
               put_int(o, "last_bid", a->last_bid);
               put_addr(o, "nft_owner", a->nft_owner);
             }));
  }
}

void store_interfaces(td::JsonValueScope *scope, const EmuTraceSnapshot &snap) {
  auto root = scope->enter_object();
  for (const auto &[addr, ifaces] : snap.interfaces) {
    root(convert::to_raw_address(addr), JsonFn([&ifaces](td::JsonValueScope *s) {
           auto per_addr = s->enter_object();
           for (const schema::BlockchainInterfaceV2 &iface : ifaces) {
             store_one_interface(per_addr, iface);
           }
         }));
  }
}

}  // namespace

td::Status write_capture(const std::string &dir, const EmuTraceSnapshot &snap,
                         int global_version) {
  EmuFinality root_finality =
      snap.nodes.empty() ? EmuFinality::emulated : snap.nodes.front().finality;

  td::JsonBuilder jb;
  {
    auto root = jb.enter_object();
    root("trace_id", snap.trace_id);
    root("tx_limit_exceeded", td::JsonBool(snap.tx_limit_exceeded));
    root("global_version", static_cast<td::int64>(global_version));
    root("nodes", JsonFn([&snap](td::JsonValueScope *s) { store_nodes(s, snap); }));
    root("interfaces", JsonFn([&snap](td::JsonValueScope *s) { store_interfaces(s, snap); }));
  }
  if (jb.string_builder().is_error()) {
    return td::Status::Error("capture JSON buffer overflow");
  }

  std::string path =
      dir + "/" + urlsafe(snap.trace_id) + "." + finality_name(root_finality) + ".json";
  return td::write_file(path, jb.string_builder().as_cslice());
}

}  // namespace mch
