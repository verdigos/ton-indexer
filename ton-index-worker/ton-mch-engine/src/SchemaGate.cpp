// Offline comparison harness for the production SchemaTraceLoader.
// HARNESS-ONLY test scaffolding: it SYNTHESIZES schema::Transactions from a
// fixture that was loaded the normal way (path A: msgpack -> our structs), runs
// them back through schema_to_trace (path B), and classifies both. `--classify`
// (A) vs `--classify-schema` (B) diffed over the corpus proves the schema loader
// reproduces the exact same trees/actions. The reverse synthesis here is NOT
// production code, production feeds real schema::Transactions from ParsedBlock.
#include "SchemaGate.h"

#include "BlockTree.h"
#include "BuildDriver.h"
#include "BuildRuntime.h"
#include "fixtures/FixtureLoader.h"
#include "fixtures/FixtureLookupSource.h"
#include "Classify.h"
#include "ClassifyCore.h"
#include "IrTables.h"
#include "ParsedBlockLookupSource.h"
#include "SchemaTraceLoader.h"

#include "IndexData.h"
#include "convert-utils.h"

#include "td/utils/base64.h"
#include "vm/boc.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace mch {

namespace {

td::Bits256 bits_from_b64(const std::string &s) {
  td::Bits256 h;
  h.set_zero();
  auto r = td::base64_decode(td::Slice(s));
  if (r.is_ok()) {
    std::string raw = r.move_as_ok();
    if (raw.size() == 32) {
      std::memcpy(h.data(), raw.data(), 32);
    }
  }
  return h;
}

schema::AccountStatus status_from_str(const std::string &s) {
  if (s == "uninit") return schema::AccountStatus::uninit;
  if (s == "frozen") return schema::AccountStatus::frozen;
  if (s == "active") return schema::AccountStatus::active;
  return schema::AccountStatus::nonexist;
}

// Minimal TransactionDescr round-tripping ONLY what schema_to_trace reads:
// the descr TYPE + the compute exit code. exit_code present -> a vm compute
// phase carrying it; absent -> a skipped phase. Other fields are defaulted.
schema::TransactionDescr descr_from(const std::string &descr,
                                    const std::optional<std::int64_t> &exit_code, bool aborted) {
  schema::TrComputePhase cp = schema::TrComputePhase_skipped{};
  if (exit_code) {
    schema::TrComputePhase_vm vm{};
    vm.exit_code = static_cast<std::int32_t>(*exit_code);
    cp = vm;
  }
  if (descr == "tick_tock") {
    schema::TransactionDescr_tick_tock tt{};
    tt.compute_ph = cp;
    tt.aborted = aborted;
    return tt;
  }
  schema::TransactionDescr_ord ord{};
  ord.compute_ph = cp;
  ord.aborted = aborted;
  return ord;
}

schema::Message synth_message(const Message &m) {
  schema::Message s;
  s.hash = bits_from_b64(m.msg_hash);
  s.source = m.source;
  s.destination = m.destination;
  if (m.opcode) s.opcode = static_cast<std::int32_t>(*m.opcode);
  if (m.value) {
    schema::CurrencyCollection cc;
    cc.grams = td::make_refint(*m.value);
    s.value = cc;
  }
  if (m.created_lt) s.created_lt = static_cast<std::uint64_t>(*m.created_lt);
  if (m.created_at) s.created_at = static_cast<std::uint32_t>(*m.created_at);
  s.bounce = m.bounce;
  s.bounced = m.bounced;
  if (m.content) s.body_boc = m.content->body;
  // map_message only carries init_state when BOTH the boc and the cell are set,
  // so the round-trip has to deserialize; a malformed boc drops the field rather
  // than failing the synth (the gate then sees the same absence on both sides).
  if (m.init_state) {
    auto r_raw = td::base64_decode(m.init_state->body);
    if (r_raw.is_ok()) {
      auto r_cell = vm::std_boc_deserialize(r_raw.move_as_ok());
      if (r_cell.is_ok()) {
        s.init_state = r_cell.move_as_ok();
        s.init_state_boc = m.init_state->body;
      }
    }
  }
  return s;
}

// Reverse of SchemaTraceLoader: our Trace -> schema::Transactions (harness).
std::vector<schema::Transaction> synth_schema_txs(const Trace &trace) {
  std::vector<schema::Transaction> out;
  for (const auto &tx : trace.transactions) {
    schema::Transaction st;
    st.hash = bits_from_b64(tx->hash);
    auto r_addr = block::StdAddress::parse(td::Slice(tx->account));
    if (r_addr.is_ok()) st.account = r_addr.move_as_ok();
    st.lt = static_cast<std::uint64_t>(tx->lt);
    st.now = static_cast<std::uint32_t>(tx->now);
    st.mc_seqno = static_cast<std::int32_t>(tx->mc_block_seqno);
    st.orig_status = status_from_str(tx->orig_status);
    st.end_status = status_from_str(tx->end_status);
    st.description =
        descr_from(tx->descr,
                   tx->compute_exit_code ? std::optional<std::int64_t>(*tx->compute_exit_code)
                                         : std::nullopt,
                   tx->aborted);
    for (const auto &m : tx->messages) {
      if (m->direction == "in") {
        st.in_msg = synth_message(*m);
      } else if (m->direction == "out") {
        st.out_msgs.push_back(synth_message(*m));
      }
    }
    out.push_back(std::move(st));
  }
  return out;
}

// Path-B TraceContext loader, injected into run_dump (lead's callback shape):
// msgpack (path A) -> synth schema::Transactions -> SchemaTraceLoader ->
// to_tree/init_block. run_dump supplies the identical rendering + engine loop,
// so path B is byte-comparable with path A and BOTH dumps (classify + actions)
// route through it. The interfaces map (lookup data) is carried from path A
// unchanged, it is the LookupSource's concern, not part of trace->tree.
td::Result<TraceContext> schema_ctx_loader(const std::string &path) {
  TRY_RESULT(ctx_a, load_trace_context(path));
  std::vector<schema::Transaction> schema_txs = synth_schema_txs(ctx_a.trace);
  TRY_RESULT(trace_b, schema_to_trace(ctx_a.trace.trace_id, schema_txs));
  TraceContext ctx_b;
  ctx_b.trace = std::move(trace_b);
  ctx_b.trace.interfaces = std::move(ctx_a.trace.interfaces);
  ctx_b.tree = to_tree(ctx_b.trace);
  if (ctx_b.tree.root == nullptr) {
    return td::Status::Error("empty event tree (path B)");
  }
  ctx_b.root = init_block(ctx_b.arena, ctx_b.tree.root);
  return ctx_b;
}

// Harness-only reverse mapping from fixture interfaces to production V2 structs.
// Kinds without a lossless V2 representation use the fixture-backed tier-2 hook.

std::optional<block::StdAddress> parse_std_addr(const Value *v) {
  if (v == nullptr || v->t != VType::Str) {
    return std::nullopt;
  }
  auto r = block::StdAddress::parse(td::Slice(v->str));
  if (r.is_error()) {
    return std::nullopt;
  }
  return r.move_as_ok();
}

block::StdAddress parse_std_addr_req(const Value *v) {
  auto o = parse_std_addr(v);
  return o ? *o : block::StdAddress{};
}

const Value *iface_field(const Value &iface_dict, const char *name, const char *field) {
  const Value *i = iface_dict.field(name);
  return i != nullptr ? i->field(field) : nullptr;
}

ParsedBlockLookupSource::InterfaceMap synth_account_interfaces(
    const std::map<std::string, Value> &fixture) {
  ParsedBlockLookupSource::InterfaceMap out;
  for (const auto &[acc_str, iface_dict] : fixture) {
    auto r_addr = block::StdAddress::parse(td::Slice(acc_str));
    if (r_addr.is_error()) {
      continue;
    }
    block::StdAddress acc = r_addr.move_as_ok();
    std::vector<schema::BlockchainInterfaceV2> ifaces;

    if (iface_dict.field("JettonWallet") != nullptr) {
      schema::JettonWalletDataV2 jw{};
      jw.balance = td::make_refint(0);  // fixture balance is float -> Null; unread
      jw.address = parse_std_addr_req(iface_field(iface_dict, "JettonWallet", "address"));
      jw.owner = parse_std_addr_req(iface_field(iface_dict, "JettonWallet", "owner"));
      jw.jetton = parse_std_addr_req(iface_field(iface_dict, "JettonWallet", "jetton"));
      ifaces.emplace_back(jw);
    }
    // NFT kinds use the fixture fallback because fixtures encode index and price
    // as floats while production V2 structs use integers. Synthesizing the V2
    // forms would change the rendered action values despite equal whole numbers.
    if (const Value *dp = iface_dict.field("DedustPool")) {
      schema::DedustPoolData p{};
      const Value *assets = dp->field("assets");
      auto slot = [&](std::size_t i) -> std::optional<block::StdAddress> {
        if (assets == nullptr || assets->t != VType::List || assets->items == nullptr ||
            assets->items->size() <= i) {
          return std::nullopt;
        }
        const Value &el = (*assets->items)[i];
        const Value *is_ton_v = el.field("is_ton");
        bool is_ton = is_ton_v != nullptr &&
                      ((is_ton_v->t == VType::Bool && is_ton_v->boolean) ||
                       (is_ton_v->t == VType::Int && !is_ton_v->num.is_null() &&
                        is_ton_v->num->sgn() != 0));
        if (is_ton) {
          return std::nullopt;  // TON slot: nullopt asset
        }
        return parse_std_addr(el.field("address"));
      };
      p.asset_1 = slot(0);
      p.asset_2 = slot(1);
      ifaces.emplace_back(p);
    }

    if (!ifaces.empty()) {
      out.emplace(acc, std::move(ifaces));
    }
  }
  return out;
}

// A self-owning LookupSource for the gate: owns the synthesized V2 map, a
// FixtureLookupSource (tier-2 fallback over the SAME fixture interfaces), and a
// ParsedBlockLookupSource delegating to both. Heap-allocated (shared_ptr) so the
// captured `this`/&map_ stay stable.
class GateProdLookupSource : public LookupSource {
 public:
  GateProdLookupSource(ParsedBlockLookupSource::InterfaceMap map,
                       const std::map<std::string, Value> *fixture_ifaces)
      : map_(std::move(map)), fix_(fixture_ifaces) {
    ParsedBlockLookupSource::Tier2Hook hook =
        [this](const std::string &kind, const std::vector<Value> &args) {
          return fix_.fetch(kind, args);
        };
    pls_ = std::make_unique<ParsedBlockLookupSource>(&map_, std::move(hook));
  }
  Value fetch(const std::string &kind, const std::vector<Value> &args) const override {
    return pls_->fetch(kind, args);
  }

 private:
  ParsedBlockLookupSource::InterfaceMap map_;
  FixtureLookupSource fix_;
  std::unique_ptr<ParsedBlockLookupSource> pls_;
};

LookupSourceFactory prod_lookup_factory() {
  return [](TraceContext &ctx) -> std::shared_ptr<LookupSource> {
    ParsedBlockLookupSource::InterfaceMap map = synth_account_interfaces(ctx.trace.interfaces);
    return std::make_shared<GateProdLookupSource>(std::move(map), &ctx.trace.interfaces);
  };
}

}  // namespace

int run_classify_schema(const std::vector<std::string> &paths) {
  return run_dump(paths, /*actions_mode=*/false, schema_ctx_loader);
}

int run_actions_schema(const std::vector<std::string> &paths) {
  return run_dump(paths, /*actions_mode=*/true, schema_ctx_loader);
}

int run_classify_prod(const std::vector<std::string> &paths) {
  // FULL production path: SchemaTraceLoader trace + ParsedBlockLookupSource
  // lookups (tier-1 synthesized V2 + tier-2 fixture fallback) vs --classify.
  return run_dump(paths, /*actions_mode=*/false, schema_ctx_loader,
                  prod_lookup_factory());
}

int run_actions_prod(const std::vector<std::string> &paths) {
  return run_dump(paths, /*actions_mode=*/true, schema_ctx_loader,
                  prod_lookup_factory());
}

}  // namespace mch
