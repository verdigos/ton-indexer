// Offline A/B gate for the production SchemaTraceLoader (Phase G step 3 slice 2).
// HARNESS-ONLY test scaffolding: it SYNTHESIZES schema::Transactions from a
// fixture that was loaded the normal way (path A: msgpack -> our structs), runs
// them back through schema_to_trace (path B), and classifies both. `--classify`
// (A) vs `--classify-schema` (B) diffed over the corpus proves the schema loader
// reproduces the exact same trees/actions. The reverse synthesis here is NOT
// production code — production feeds real schema::Transactions from ParsedBlock.
#include "SchemaGate.h"

#include "BlockTree.h"
#include "BuildDriver.h"
#include "Classify.h"
#include "ClassifyCore.h"
#include "IrLoader.h"
#include "SchemaTraceLoader.h"

#include "IndexData.h"
#include "convert-utils.h"

#include "td/utils/base64.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
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
// unchanged — it is the LookupSource's concern, not part of trace->tree.
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

}  // namespace

int run_classify_schema(const std::string &artifact_path, const std::vector<std::string> &paths) {
  return run_dump(artifact_path, paths, /*actions_mode=*/false, schema_ctx_loader);
}

int run_actions_schema(const std::string &artifact_path, const std::vector<std::string> &paths) {
  return run_dump(artifact_path, paths, /*actions_mode=*/true, schema_ctx_loader);
}

}  // namespace mch
