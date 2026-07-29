// EM2 W7(iii) — the ONLY TU that knows the emulator's Redis wire format.
//
// Serializer.hpp is included READ-ONLY, exactly as mch-emu-bridge consumes
// TraceEmulator.h: the structs there ARE the contract, so decoding with a
// private copy of them would test this file against itself instead of against
// what TraceInserter writes. The C++ reader precedent is
// TraceInserter.cpp:325 (`load_existing_node`), which unpacks a RedisTraceNode
// off the same field.
//
// ONE exception, forced upstream: `AddressInterfaces` cannot be decoded with the
// real structs, because msgpack-utils.h's convert<td::RefInt256> is a hard
// `throw std::runtime_error("Deserializion of td::RefInt256 is not
// implemented")`. Its PACK side writes `to_dec_string()`, so the interface
// mirrors below differ from Serializer.hpp's structs in exactly one respect —
// every RefInt256 field is read back as the decimal std::string it was written
// as. Field NAMES are what the wire is keyed by (ton-trace-emulator is built
// with -DMSGPACK_USE_DEFINE_MAP, so MSGPACK_DEFINE emits maps, not arrays), and
// the mirrors keep them; --make-dump packs with the REAL AddressInterfaces, so
// any name drift breaks the corpus round-trip loudly instead of silently.
#include "RedisReplay.h"

#include "Serializer.hpp"  // ton-trace-emulator: RedisTraceNode, AddressInterfaces

#include "td/utils/base64.h"

#include <cmath>
#include <cstring>
#include <deque>
#include <optional>
#include <set>
#include <sstream>
#include <variant>

namespace mch {

namespace {

// --- interface mirrors (see the file header for why they exist) --------------
// Field names and, crucially, the VARIANT ORDER must match Serializer.hpp:
// AddressInterfaces packs `std::variant` as [alternative_index, value]
// (msgpack cpp17/variant.hpp), so a reordered alternative list silently decodes
// one interface as another.

struct RJettonWallet {
  std::string balance;  // RefInt256 on the wire -> decimal string
  block::StdAddress address;
  block::StdAddress owner;
  block::StdAddress jetton;
  MSGPACK_DEFINE(balance, address, owner, jetton);
};

struct RJettonMaster {
  block::StdAddress address;
  MSGPACK_DEFINE(address);
};

struct RNftItem {
  block::StdAddress address;
  bool init{false};
  std::string index;  // RefInt256 -> decimal string
  std::optional<block::StdAddress> collection_address;
  std::optional<block::StdAddress> owner_address;
  // Same type on both sides, so unlike `index` this needs no adaptor. READ, not
  // skipped, since the tier-1 content fix landed: get_nft_data FAULTS on a
  // non-Dict content (HostNft.cpp:230-233), so dropping it here would keep
  // rejecting the nft rows production now classifies.
  std::optional<std::map<std::string, std::string>> content;
  MSGPACK_DEFINE(address, init, index, collection_address, owner_address, content);
};

struct RNftCollection {
  block::StdAddress address;
  MSGPACK_DEFINE(address);
};

struct RGetGemsSale {
  block::StdAddress address;
  bool is_complete{false};
  std::uint32_t created_at{0};
  block::StdAddress marketplace_address;
  block::StdAddress nft_address;
  std::optional<block::StdAddress> nft_owner_address;
  std::string full_price;
  block::StdAddress marketplace_fee_address;
  std::string marketplace_fee;
  block::StdAddress royalty_address;
  std::string royalty_amount;
  MSGPACK_DEFINE(address, is_complete, created_at, marketplace_address, nft_address,
                 nft_owner_address, full_price, marketplace_fee_address, marketplace_fee,
                 royalty_address, royalty_amount);
};

struct RGetGemsAuction {
  block::StdAddress address;
  bool end{false};
  std::uint32_t end_time{0};
  block::StdAddress mp_addr;
  block::StdAddress nft_addr;
  std::optional<block::StdAddress> nft_owner;
  std::string last_bid;
  std::optional<block::StdAddress> last_member;
  std::uint32_t min_step{0};
  block::StdAddress mp_fee_addr;
  std::uint32_t mp_fee_factor{0}, mp_fee_base{0};
  block::StdAddress royalty_fee_addr;
  std::uint32_t royalty_fee_factor{0}, royalty_fee_base{0};
  std::string max_bid;
  std::string min_bid;
  std::uint32_t created_at{0};
  std::uint32_t last_bid_at{0};
  bool is_canceled{false};
  MSGPACK_DEFINE(address, end, end_time, mp_addr, nft_addr, nft_owner, last_bid, last_member,
                 min_step, mp_fee_addr, mp_fee_factor, mp_fee_base, royalty_fee_addr,
                 royalty_fee_factor, royalty_fee_base, max_bid, min_bid, created_at, last_bid_at,
                 is_canceled);
};

struct RAddressInterfaces {
  std::vector<std::variant<RJettonWallet, RJettonMaster, RNftItem, RNftCollection, RGetGemsSale,
                           RGetGemsAuction>>
      interfaces;
  MSGPACK_DEFINE(interfaces);
};

// A decimal string from the wire -> RefInt256. An unparseable/absent value is a
// null RefInt256, which tier1 already renders as a Null field.
td::RefInt256 refint(const std::string &dec) {
  return dec.empty() ? td::RefInt256{} : td::dec_string_to_int256(td::Slice(dec));
}

schema::AccountStatus status_of(::AccountStatus s) {
  switch (s) {
    case ::AccountStatus::uninit:
      return schema::AccountStatus::uninit;
    case ::AccountStatus::frozen:
      return schema::AccountStatus::frozen;
    case ::AccountStatus::active:
      return schema::AccountStatus::active;
    case ::AccountStatus::nonexist:
      return schema::AccountStatus::nonexist;
  }
  return schema::AccountStatus::nonexist;
}

::AccountStatus status_to_redis(const std::string &s) {
  if (s == "uninit") return ::AccountStatus::uninit;
  if (s == "frozen") return ::AccountStatus::frozen;
  if (s == "active") return ::AccountStatus::active;
  return ::AccountStatus::nonexist;
}

// --- RedisTraceNode -> schema ------------------------------------------------
//
// Field-for-field. The three CHOICES here (each one a place the wire is poorer
// than the in-process converter):
//   1. `value`: uint64 grams -> CurrencyCollection{grams}, extra_currencies left
//      EMPTY. The emulator's Message has no extra-currency field at all, so this
//      is a loss of the wire, not of this code (emu-integration-plan.md §2.2).
//      SchemaTraceLoader reads `has_extra_currencies` off it, so a replayed
//      trace always reports "no extra currencies".
//   2. `extra_flags`: optional<std::string> on the wire vs optional<RefInt256> in
//      schema::Message. Not read by SchemaTraceLoader (nor by any matcher), and
//      no meaning-preserving conversion exists, so it is DROPPED rather than
//      guessed at.
//   3. `description`: the wire type is TransactionDescr_ord — an alias, not a
//      variant — so tick-tock transactions cannot round-trip and every replayed
//      tx reads as "ord". Only compute_ph + aborted are carried, which is
//      exactly what SchemaTraceLoader::descr_info consumes; storage/credit/
//      action/bounce phases are left default because nothing downstream reads
//      them.
schema::Message message_of(const ::Message &m) {
  schema::Message s;
  s.hash = m.hash;
  s.hash_norm = m.hash_norm;
  s.source = m.source;
  s.destination = m.destination;
  if (m.value) {
    schema::CurrencyCollection cc;
    cc.grams = td::make_refint(static_cast<long long>(*m.value));
    s.value = cc;  // (1) extra_currencies stays empty
  }
  if (m.fwd_fee) s.fwd_fee = td::make_refint(static_cast<long long>(*m.fwd_fee));
  if (m.ihr_fee) s.ihr_fee = td::make_refint(static_cast<long long>(*m.ihr_fee));
  if (m.import_fee) s.import_fee = td::make_refint(static_cast<long long>(*m.import_fee));
  // (2) m.extra_flags is dropped.
  s.created_lt = m.created_lt;
  s.created_at = m.created_at;
  s.opcode = m.opcode;
  s.ihr_disabled = m.ihr_disabled;
  s.bounce = m.bounce;
  s.bounced = m.bounced;
  s.body_boc = m.body_boc;      // the load-bearing one: MsgParse decodes this
  s.init_state_boc = m.init_state_boc;
  return s;
}

schema::Transaction transaction_of(const ::RedisTraceNode &node) {
  const ::Transaction &t = node.transaction;
  schema::Transaction s;
  s.hash = t.hash;
  s.account = t.account;
  s.lt = t.lt;
  s.prev_trans_hash = t.prev_trans_hash;
  s.prev_trans_lt = t.prev_trans_lt;
  s.now = t.now;
  // mc_seqno comes off the WRAPPER, not the transaction — same as the in-process
  // converter (EmuTraceConvert.cpp: tx.mc_seqno = node.mc_seqno).
  s.mc_seqno = node.mc_block_seqno;
  s.orig_status = status_of(t.orig_status);
  s.end_status = status_of(t.end_status);
  s.total_fees.grams = td::make_refint(static_cast<long long>(t.total_fees));
  s.account_state_hash_before = t.account_state_hash_before;
  s.account_state_hash_after = t.account_state_hash_after;
  if (t.in_msg) {
    s.in_msg = message_of(*t.in_msg);
  }
  for (const ::Message &om : t.out_msgs) {
    s.out_msgs.push_back(message_of(om));
  }
  // (3) ord-only, compute phase + aborted.
  schema::TransactionDescr_ord ord{};
  ord.credit_first = t.description.credit_first;
  ord.aborted = t.description.aborted;
  ord.destroyed = t.description.destroyed;
  if (const auto *vm = std::get_if<::TrComputePhase_vm>(&t.description.compute_ph)) {
    schema::TrComputePhase_vm cv{};
    cv.success = vm->success;
    cv.msg_state_used = vm->msg_state_used;
    cv.account_activated = vm->account_activated;
    cv.gas_fees = td::make_refint(static_cast<long long>(vm->gas_fees));
    cv.gas_used = vm->gas_used;
    cv.gas_limit = vm->gas_limit;
    cv.gas_credit = vm->gas_credit;
    cv.mode = vm->mode;
    cv.exit_code = vm->exit_code;  // the one SchemaTraceLoader reads
    cv.exit_arg = vm->exit_arg;
    cv.vm_steps = vm->vm_steps;
    cv.vm_init_state_hash = vm->vm_init_state_hash;
    cv.vm_final_state_hash = vm->vm_final_state_hash;
    ord.compute_ph = cv;
  } else {
    schema::TrComputePhase_skipped cs{};
    if (const auto *sk = std::get_if<::TrComputePhase_skipped>(&t.description.compute_ph)) {
      cs.reason = static_cast<schema::ComputeSkipReason>(sk->reason);
    }
    ord.compute_ph = cs;
  }
  s.description = ord;
  return s;
}

// --- AddressInterfaces -> the tier-1 V2 shapes -------------------------------
//
// The SAME four adaptations mch-emu already performs in-process
// (emu/EmuInterfaces.cpp), against the same tier-1 consumers
// (ParsedBlockLookupSource::tier1). Two consequences to state plainly rather
// than discover later:
//   - JettonMaster / NftCollection map to no tier-1 lookup kind and are
//     DROPPED (counted). Their V2 twins exist, but no `lookup` kind reads them.
//   - dedust_pool, nominator_pool and jvault_assets have NO representation in
//     AddressInterfaces at all, so they are permanent replay misses. That is not
//     a gap in this adapter: the in-process EM path has exactly the same four
//     to_v2 overloads, so a replayed trace sees precisely what EM2 will see in
//     production.
bool push_v2(std::vector<schema::BlockchainInterfaceV2> &out,
             const std::variant<RJettonWallet, RJettonMaster, RNftItem, RNftCollection,
                                RGetGemsSale, RGetGemsAuction> &v) {
  if (const auto *r = std::get_if<RJettonWallet>(&v)) {
    schema::JettonWalletDataV2 d{};
    d.balance = refint(r->balance);
    d.address = r->address;
    d.owner = r->owner;
    d.jetton = r->jetton;
    out.emplace_back(d);
    return true;
  }
  if (const auto *r = std::get_if<RNftItem>(&v)) {
    schema::NFTItemDataV2 d{};
    d.address = r->address;
    d.init = r->init;
    d.index = refint(r->index);
    d.collection_address = r->collection_address;
    d.owner_address = r->owner_address;
    // Carried, matching EmuInterfaces::to_v2 now that it carries it too. A
    // nullopt stays nullopt on purpose: tier 1 renders that as Null and
    // get_nft_data rejects, which is the same outcome Python takes for a
    // content-less item — synthesizing an empty dict here would fabricate a row.
    d.content = r->content;
    out.emplace_back(d);
    return true;
  }
  if (const auto *r = std::get_if<RGetGemsSale>(&v)) {
    schema::GetGemsNftFixPriceSaleData d{};
    d.address = r->address;
    d.is_complete = r->is_complete;
    d.created_at = r->created_at;
    d.marketplace_address = r->marketplace_address;
    d.nft_address = r->nft_address;
    d.nft_owner_address = r->nft_owner_address;
    d.full_price = refint(r->full_price);
    d.marketplace_fee_address = r->marketplace_fee_address;
    d.marketplace_fee = refint(r->marketplace_fee);
    d.royalty_address = r->royalty_address;
    d.royalty_amount = refint(r->royalty_amount);
    out.emplace_back(d);
    return true;
  }
  if (const auto *r = std::get_if<RGetGemsAuction>(&v)) {
    schema::GetGemsNftAuctionData d{};
    d.address = r->address;
    d.end = r->end;
    d.end_time = r->end_time;
    d.mp_addr = r->mp_addr;
    d.nft_addr = r->nft_addr;
    d.nft_owner = r->nft_owner;
    d.last_bid = refint(r->last_bid);
    d.last_member = r->last_member;
    d.min_step = r->min_step;
    d.mp_fee_addr = r->mp_fee_addr;
    d.mp_fee_factor = r->mp_fee_factor;
    d.mp_fee_base = r->mp_fee_base;
    d.royalty_fee_addr = r->royalty_fee_addr;
    d.royalty_fee_factor = r->royalty_fee_factor;
    d.royalty_fee_base = r->royalty_fee_base;
    d.max_bid = refint(r->max_bid);
    d.min_bid = refint(r->min_bid);
    d.created_at = r->created_at;
    d.last_bid_at = r->last_bid_at;
    d.is_canceled = r->is_canceled;
    out.emplace_back(d);
    return true;
  }
  return false;  // JettonMaster / NftCollection: no tier-1 kind reads them
}

std::string node_key_of(const td::Bits256 &msg_hash) {
  return td::base64_encode(msg_hash.as_slice());
}

}  // namespace

td::Result<RedisReplayTrace> redis_dump_to_replay(const RedisTraceDump &dump) {
  RedisReplayTrace out;

  if (dump.root_node.empty()) {
    return td::Status::Error("no root_node field");
  }
  if (dump.nodes.find(dump.root_node) == dump.nodes.end()) {
    return td::Status::Error("root_node " + dump.root_node + " is not among the node blobs");
  }

  // Decode every node blob once, keyed the way the hash keys them.
  std::map<std::string, ::RedisTraceNode> decoded;
  for (const auto &[key, blob] : dump.nodes) {
    ::RedisTraceNode node;
    try {
      msgpack::unpacked r;
      msgpack::unpack(r, blob.data(), blob.size());
      r.get().convert(node);
    } catch (const std::exception &) {
      out.nodes_undecodable++;
      continue;
    }
    if (!node.transaction.in_msg) {
      // Every Redis node key IS its in-msg hash, so a node without one cannot be
      // addressed and cannot be a child of anything.
      out.nodes_undecodable++;
      continue;
    }
    decoded.emplace(key, std::move(node));
  }
  if (decoded.find(dump.root_node) == decoded.end()) {
    return td::Status::Error("root node blob " + dump.root_node + " did not decode");
  }

  // Breadth-first from the root over out-message hashes — the traversal
  // TraceInserter itself uses to flatten a trace (TraceInserter.cpp:71-108).
  std::set<std::string> seen;
  std::deque<std::string> queue{dump.root_node};
  seen.insert(dump.root_node);
  while (!queue.empty()) {
    const ::RedisTraceNode &node = decoded.at(queue.front());
    queue.pop_front();
    out.txs.push_back(transaction_of(node));
    out.node_finality.push_back(static_cast<std::uint8_t>(node.finality));
    for (const ::Message &om : node.transaction.out_msgs) {
      std::string child = node_key_of(om.hash);
      if (decoded.count(child) != 0 && seen.insert(child).second) {
        queue.push_back(child);
      }
    }
  }
  // A node blob the root cannot reach is dropped, not appended: it belongs to a
  // superseded emission or a broken hash, and feeding it to to_tree would either
  // be relinked into the wrong place or counted as an orphan. Either way it is a
  // fact about the sweep, so it is reported.
  out.unreachable_nodes = decoded.size() - out.txs.size();

  for (const auto &[acc, blob] : dump.interfaces) {
    auto r_addr = block::StdAddress::parse(td::Slice(acc));
    if (r_addr.is_error()) {
      continue;  // not an account field (root_node, depth_limit_exceeded, otel, ...)
    }
    RAddressInterfaces ifaces;
    try {
      msgpack::unpacked r;
      msgpack::unpack(r, blob.data(), blob.size());
      r.get().convert(ifaces);
    } catch (const std::exception &) {
      out.iface_blobs_undecodable++;
      continue;
    }
    std::vector<schema::BlockchainInterfaceV2> v2;
    for (const auto &iface : ifaces.interfaces) {
      if (!push_v2(v2, iface)) {
        out.iface_variants_dropped++;
      }
    }
    if (!v2.empty()) {
      out.ifaces.emplace(r_addr.move_as_ok(), std::move(v2));
    }
  }

  return out;
}

// --- --make-dump: fixture -> the prod sweep's shape ---------------------------

namespace {

td::Bits256 bits_from_b64(const std::string &s) {
  td::Bits256 h;
  h.set_zero();
  auto r = td::base64_decode(td::Slice(s));
  if (r.is_ok() && r.ok().size() == 32) {
    std::memcpy(h.data(), r.ok().data(), 32);
  }
  return h;
}

::Message synth_message(const Message &m) {
  // VALUE-INITIALIZED, not default-initialized. Serializer.hpp's structs are
  // aggregates with no member initializers, and MSGPACK_DEFINE serializes EVERY
  // field — including the ones this synthesis has no source for (fwd_fee,
  // ihr_fee, hashes, total_fees, block_id). Left default, those are indeterminate
  // and go onto the wire as whatever was on the stack, which made --make-dump
  // non-reproducible across rebuilds. Classification never read them, so every
  // gate stayed green while the dump bytes moved.
  ::Message s{};
  s.hash = bits_from_b64(m.msg_hash);
  s.source = m.source;
  s.destination = m.destination;
  if (m.value) s.value = static_cast<std::uint64_t>(*m.value);
  if (m.created_lt) s.created_lt = static_cast<std::uint64_t>(*m.created_lt);
  if (m.created_at) s.created_at = static_cast<std::uint32_t>(*m.created_at);
  if (m.opcode) s.opcode = static_cast<std::int32_t>(*m.opcode);
  s.bounce = m.bounce;
  s.bounced = m.bounced;
  if (m.content) s.body_boc = m.content->body;
  return s;
}

// The fixture Value shapes FixtureLookupSource reads, re-packed into the
// emulator's interface structs. Numeric fields are RefInt256 on the wire, and a
// fixture stores some of them as msgpack FLOATS (VType::Float) — an artefact of
// the Python recorder, called out at SchemaGate.cpp:205-224. There is no
// lossless float->RefInt256 conversion, so the dry run trades those few fields
// for the real wire type; the diff report names every row it costs.
td::RefInt256 num_of(const Value *v) {
  if (v == nullptr) return {};
  if (v->t == VType::Int || v->t == VType::Amount) return v->num;
  if (v->t == VType::Float) {
    // NOT via `long long`: real nft_item_index values reach ~1e76 (fragment
    // usernames index by hash), and that cast is undefined — in practice it
    // saturates to LLONG_MIN, so the dump would carry -9223372036854775808 as
    // the index. Printing the double at full integer precision and parsing it as
    // a 256-bit int is exact, because a double of that magnitude IS an integer.
    if (!std::isfinite(v->dnum)) {
      return {};
    }
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%.0f", v->dnum);
    return td::dec_string_to_int256(std::string(buf));
  }
  return {};
}

std::optional<block::StdAddress> addr_of(const Value *v) {
  if (v == nullptr || v->t != VType::Str) return std::nullopt;
  auto r = block::StdAddress::parse(td::Slice(v->str));
  if (r.is_error()) return std::nullopt;
  return r.move_as_ok();
}

// A REQUIRED address field. The fallback matters: a default-constructed
// block::StdAddress carries workchain == ton::workchainInvalid, which packs as
// "-2147483648:00…0" and then fails StdAddress::parse on the way back in,
// throwing out the whole interface blob. Fixtures only store the fields Python's
// TestInterfaceRepository serves, so the GetGems fee/royalty addresses are
// routinely absent; a real emulator always has them.
block::StdAddress addr_req(const Value *v, const block::StdAddress &fallback) {
  auto a = addr_of(v);
  return a ? *a : fallback;
}

const Value *sub(const Value &dict, const char *iface, const char *field) {
  const Value *i = dict.field(iface);
  return i != nullptr ? i->field(field) : nullptr;
}

// The fixture's nft `content` Dict -> the FLAT map<string,string> the wire
// carries. Flat is not a simplification: NftItemDetectorR::Result::content is
// `optional<map<string,string>>` by construction, so a real emulator can only
// ever have produced a flat map. Non-Str leaves are therefore a recorder
// artefact and are SKIPPED rather than stringified — inventing a string for one
// would put a value on the wire that no emulator would emit. (Verified against
// the corpus: every nft item's content is flat str->str, e.g. {"uri": ...}.)
std::optional<std::map<std::string, std::string>> content_map(const Value *v) {
  if (v == nullptr || v->t != VType::Dict || v->fields == nullptr) {
    return std::nullopt;
  }
  std::map<std::string, std::string> out;
  for (const auto &[key, val] : *v->fields) {
    if (val.t == VType::Str) {
      out.emplace(key, val.str);
    }
  }
  return out;
}

}  // namespace

td::Result<RedisTraceDump> fixture_to_redis_dump(const std::string &trace_key,
                                                 const Trace &trace) {
  RedisTraceDump dump;
  dump.trace_key = trace_key;
  dump.trace_id = trace.trace_id;

  // The root is the lowest-lt transaction with an in-message — the same node
  // to_tree walks up to, and the node whose key TraceInserter stores as
  // `root_node` (both are base64(ext_in_msg_hash)).
  const Transaction *root = nullptr;
  for (const auto &tx : trace.transactions) {
    ::Transaction st{};  // see synth_message: value-init or ship stack garbage
    st.hash = bits_from_b64(tx->hash);
    auto r_addr = block::StdAddress::parse(td::Slice(tx->account));
    if (r_addr.is_ok()) st.account = r_addr.move_as_ok();
    st.lt = static_cast<std::uint64_t>(tx->lt);
    st.now = static_cast<std::uint32_t>(tx->now);
    st.orig_status = status_to_redis(tx->orig_status);
    st.end_status = status_to_redis(tx->end_status);
    st.description.aborted = tx->aborted;
    if (tx->compute_exit_code) {
      ::TrComputePhase_vm vm{};
      vm.exit_code = static_cast<std::int32_t>(*tx->compute_exit_code);
      st.description.compute_ph = vm;
    } else {
      st.description.compute_ph = ::TrComputePhase_skipped{};
    }
    const Message *in_msg = nullptr;
    for (const auto &m : tx->messages) {
      if (m->direction == "in") {
        in_msg = m.get();
        st.in_msg = synth_message(*m);
      } else if (m->direction == "out") {
        st.out_msgs.push_back(synth_message(*m));
      }
    }
    if (in_msg == nullptr) {
      continue;  // unaddressable in Redis; the emulator never emits such a node
    }
    if (root == nullptr || tx->lt < root->lt) {
      root = tx.get();
      dump.root_node = in_msg->msg_hash;  // fixtures already store it base64'd
    }
    ::RedisTraceNode node{};  // block_id in particular has no source here
    node.transaction = std::move(st);
    node.emulated = false;
    node.mc_block_seqno = static_cast<std::uint32_t>(tx->mc_block_seqno);
    node.finality = FinalityState::Finalized;
    std::stringstream buf;
    msgpack::pack(buf, node);
    dump.nodes.emplace(in_msg->msg_hash, buf.str());
  }
  if (root == nullptr) {
    return td::Status::Error("fixture has no transaction with an in-message");
  }

  for (const auto &[acc, iface_dict] : trace.interfaces) {
    // Every interface belongs to the account it is filed under, so that account
    // is the honest fallback for a missing `address` and a decodable filler for
    // the fee/royalty addresses fixtures do not carry.
    block::StdAddress self;
    if (auto r = block::StdAddress::parse(td::Slice(acc)); r.is_ok()) {
      self = r.move_as_ok();
    } else {
      self = block::StdAddress(0, ton::StdSmcAddress{});
    }
    ::AddressInterfaces ifaces;
    if (iface_dict.field("JettonWallet") != nullptr) {
      ::JettonWalletSchema s{};
      s.balance = num_of(sub(iface_dict, "JettonWallet", "balance"));
      if (s.balance.is_null()) s.balance = td::make_refint(0);
      s.address = addr_req(sub(iface_dict, "JettonWallet", "address"), self);
      s.owner = addr_req(sub(iface_dict, "JettonWallet", "owner"), self);
      s.jetton = addr_req(sub(iface_dict, "JettonWallet", "jetton"), self);
      ifaces.interfaces.push_back(s);
    }
    if (iface_dict.field("NftItem") != nullptr) {
      ::NftItemSchema s{};
      s.address = addr_req(sub(iface_dict, "NftItem", "address"), self);
      const Value *init = sub(iface_dict, "NftItem", "init");
      s.init = init != nullptr && init->t == VType::Bool && init->boolean;
      s.index = num_of(sub(iface_dict, "NftItem", "index"));
      if (s.index.is_null()) s.index = td::make_refint(0);
      s.collection_address = addr_of(sub(iface_dict, "NftItem", "collection_address"));
      s.owner_address = addr_of(sub(iface_dict, "NftItem", "owner_address"));
      s.content = content_map(sub(iface_dict, "NftItem", "content"));
      ifaces.interfaces.push_back(s);
    }
    if (iface_dict.field("NftSale") != nullptr) {
      // Value-init: the GetGems schemas carry a dozen numeric fields the fixture
      // has no source for (created_at, end_time, min_step, the fee factors), and
      // MSGPACK_DEFINE ships every one of them.
      ::GetGemsNftFixPriceSaleSchema s{};
      s.address = addr_req(sub(iface_dict, "NftSale", "address"), self);
      s.marketplace_address = addr_req(sub(iface_dict, "NftSale", "marketplace_address"), self);
      s.nft_address = addr_req(sub(iface_dict, "NftSale", "nft_address"), self);
      s.nft_owner_address = addr_of(sub(iface_dict, "NftSale", "nft_owner_address"));
      s.full_price = num_of(sub(iface_dict, "NftSale", "full_price"));
      if (s.full_price.is_null()) s.full_price = td::make_refint(0);
      s.marketplace_fee_address = addr_req(sub(iface_dict, "NftSale", "marketplace_fee_address"), self);
      s.royalty_address = addr_req(sub(iface_dict, "NftSale", "royalty_address"), self);
      s.marketplace_fee = td::make_refint(0);
      s.royalty_amount = td::make_refint(0);
      ifaces.interfaces.push_back(s);
    }
    if (iface_dict.field("NftAuction") != nullptr) {
      ::GetGemsNftAuctionSchema s{};
      s.address = addr_req(sub(iface_dict, "NftAuction", "address"), self);
      s.mp_addr = addr_req(sub(iface_dict, "NftAuction", "mp_addr"), self);
      s.nft_addr = addr_req(sub(iface_dict, "NftAuction", "nft_addr"), self);
      s.nft_owner = addr_of(sub(iface_dict, "NftAuction", "nft_owner"));
      s.last_bid = num_of(sub(iface_dict, "NftAuction", "last_bid"));
      if (s.last_bid.is_null()) s.last_bid = td::make_refint(0);
      s.mp_fee_addr = addr_req(sub(iface_dict, "NftAuction", "mp_fee_addr"), self);
      s.royalty_fee_addr = addr_req(sub(iface_dict, "NftAuction", "royalty_fee_addr"), self);
      s.max_bid = td::make_refint(0);
      s.min_bid = td::make_refint(0);
      ifaces.interfaces.push_back(s);
    }
    if (ifaces.interfaces.empty()) {
      continue;
    }
    std::stringstream buf;
    msgpack::pack(buf, ifaces);
    dump.interfaces.emplace(acc, buf.str());
  }

  return dump;
}

}  // namespace mch
