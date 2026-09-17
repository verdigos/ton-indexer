// Classify-core gate: topology guards, fallback and owned schema outcomes.
#include "ClassifyCoreTest.h"

#include "BlockTree.h"
#include "ClassifyCore.h"
#include "EnginePrep.h"
#include "HostRegistry.h"
#include "MsgParse.h"
#include "ParsedBlockLookupSource.h"
#include "IrTables.h"
#include "TraceLoader.h"
#include "fixtures/FixtureLookupSource.h"
#include "fixtures/FixtureLoader.h"

#include "td/utils/base64.h"
#include "vm/cells/CellBuilder.h"
#include "vm/excno.hpp"

#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace mch {
namespace {

int g_fail = 0;

void check(const std::string &name, bool ok) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
  if (!ok) g_fail++;
}

enum class HostThrow { vm_error, std_error, bad_alloc, no_gas, virt_error, unknown };
HostThrow g_serious_kind = HostThrow::bad_alloc;

class ThrowingLookupTable final : public LookupTable {
 public:
  explicit ThrowingLookupTable(HostThrow kind) : kind_(kind) {
  }

  bool has(const std::string &) const override { return true; }

  Value get(const std::string &, const std::vector<Value> &) const override {
    switch (kind_) {
      case HostThrow::vm_error: throw vm::VmError(vm::Excno::cell_und, "host vm error");
      case HostThrow::std_error: throw std::runtime_error("host standard error");
      case HostThrow::bad_alloc: throw std::bad_alloc();
      case HostThrow::no_gas: throw vm::VmNoGas();
      case HostThrow::virt_error: throw vm::VmVirtError();
      case HostThrow::unknown: throw 7;
    }
    return Value::null();
  }

 private:
  HostThrow kind_;
};

TraceContext make_host_test_context(const std::string &trace_id) {
  TraceContext ctx;
  ctx.trace.trace_id = trace_id;
  auto tx = std::make_unique<Transaction>();
  tx->hash = trace_id + "-tx";
  tx->lt = 100;
  tx->now = 200;
  tx->mc_block_seqno = 300;
  tx->account = "0:" + std::string(64, 'B');
  tx->descr = "ord";
  tx->orig_status = "active";
  tx->end_status = "active";
  auto in = std::make_unique<Message>();
  in->msg_hash = trace_id + "-message";
  in->tx_hash = tx->hash;
  in->tx_lt = tx->lt;
  in->direction = "in";
  in->source = "0:" + std::string(64, 'A');
  in->destination = tx->account;
  in->opcode = 0x11223344;
  in->value = 1000;
  in->tx = tx.get();
  tx->messages.push_back(std::move(in));
  ctx.trace.transactions.push_back(std::move(tx));
  ctx.tree = to_tree(ctx.trace);
  ctx.root = init_block(ctx.arena, ctx.tree.root);
  return ctx;
}

EvalResult call_throwing_host(BuildEnv &env, HostThrow kind) {
  ThrowingLookupTable lookups(kind);
  BuildEnv host_env = env;
  host_env.lookups = &lookups;
  return rt_call_hostfn(host_env, "auction_bid_data", {Value::make_block(env.anchor)});
}

CompiledMatcher test_matcher(std::string name, int artifact_index, bool calls_host = false) {
  CompiledMatcher matcher;
  matcher.name = std::move(name);
  matcher.artifact_index = artifact_index;
  matcher.produces = {"call_contract"};
  matcher.anchor_kind = AnchorKind::BType;
  matcher.anchor_btypes = {"call_contract"};
  CompiledNode node;
  node.kind = NodeKind::BlockType;
  node.btype = "call_contract";
  matcher.nodes.push_back(std::move(node));
  if (calls_host) matcher.ref_fns.insert("auction_bid_data");
  return matcher;
}

int g_host_reject_calls = 0;
int g_parse_reject_calls = 0;
int g_success_calls = 0;
bool g_rejected_anchor_stayed_unconsumed = true;

BuildOutcome host_reject_build(BuildEnv &env) {
  g_host_reject_calls++;
  g_rejected_anchor_stayed_unconsumed &= env.anchor->parent == nullptr;
  EvalResult result = call_throwing_host(env, HostThrow::std_error);
  return result.faulted ? BuildOutcome::rejected(result.message) : BuildOutcome::accepted();
}

BuildOutcome required_parse_reject_build(BuildEnv &env) {
  g_parse_reject_calls++;
  g_rejected_anchor_stayed_unconsumed &= env.anchor->parent == nullptr;
  const Value malformed = Value::make_cell(vm::CellBuilder().finalize());
  EvalResult required = rt_parse_expr(malformed, {"ChangeDnsRecordMessage"}, false);
  return required.faulted ? BuildOutcome::rejected(required.message) : BuildOutcome::accepted();
}

BuildOutcome successful_build(BuildEnv &env) {
  g_success_calls++;
  g_rejected_anchor_stayed_unconsumed &= env.anchor->parent == nullptr;
  return BuildOutcome::accepted();
}

BuildOutcome serious_host_build(BuildEnv &env) {
  EvalResult result = call_throwing_host(env, g_serious_kind);
  return result.faulted ? BuildOutcome::rejected(result.message) : BuildOutcome::accepted();
}

void test_host_fn_exception_is_clean_rejection() {
  TraceContext ctx = make_host_test_context("host-clean-rejection");
  BuildEnv env;
  env.anchor = ctx.root;
  for (HostThrow kind : {HostThrow::vm_error, HostThrow::std_error}) {
    const std::string name = kind == HostThrow::vm_error ? "vm_error" : "std_error";
    bool threw = false;
    bool faulted = false;
    try {
      faulted = call_throwing_host(env, kind).faulted;
    } catch (...) {
      threw = true;
    }
    check("host_fn/clean_rejection/" + name + "/no_throw", !threw);
    check("host_fn/clean_rejection/" + name + "/faulted", faulted);
  }
}

void test_host_fn_error_rejects_attempt_and_next_matcher_runs() {
  TraceContext ctx = make_host_test_context("host-rejection-continuation");
  std::vector<CompiledMatcher> matchers{
      test_matcher("host-error", 0, true),
      test_matcher("required-parse-error", 1),
      test_matcher("success", 2),
  };
  ClassifySetup setup;
  setup.included = {0, 1, 2};
  setup.build_fns.emplace(0, host_reject_build);
  setup.build_fns.emplace(1, required_parse_reject_build);
  setup.build_fns.emplace(2, successful_build);
  FixtureLookupSource src(&ctx.trace.interfaces);
  g_host_reject_calls = 0;
  g_parse_reject_calls = 0;
  g_success_calls = 0;
  g_rejected_anchor_stayed_unconsumed = true;

  ClassifyResult result = classify_trace(ctx, matchers, setup, src);
  check("host_fn/continuation/no_trace_failure", !result.failure);
  check("host_fn/continuation/no_fallback", result.fallback_rows.empty());
  check("host_fn/continuation/host_attempted", g_host_reject_calls > 0);
  check("host_fn/continuation/required_parse_attempted", g_parse_reject_calls == 1);
  check("host_fn/continuation/later_matcher_ran", g_success_calls == 1);
  check("host_fn/continuation/rejections_did_not_consume",
        g_rejected_anchor_stayed_unconsumed);
  check("host_fn/continuation/success_action_preserved",
        result.actions.size() == 1 && result.actions.front().matcher_name == "success" &&
            result.actions.front().anchor == ctx.root &&
            ctx.root->parent == result.actions.front().produced);
}

void test_host_fn_serious_exception_stays_trace_failure() {
  struct Case {
    const char *name;
    HostThrow kind;
  };
  const std::vector<Case> cases{{"bad_alloc", HostThrow::bad_alloc},
                                {"no_gas", HostThrow::no_gas},
                                {"virt_error", HostThrow::virt_error},
                                {"unknown", HostThrow::unknown}};
  for (const Case &test : cases) {
    TraceContext ctx = make_host_test_context(std::string("host-classify-") + test.name);
    std::vector<CompiledMatcher> matchers{test_matcher("serious-host-error", 0, true)};
    ClassifySetup setup;
    setup.included = {0};
    g_serious_kind = test.kind;
    setup.build_fns.emplace(0, serious_host_build);
    FixtureLookupSource src(&ctx.trace.interfaces);
    ClassifyResult result = classify_trace(ctx, matchers, setup, src);
    check(std::string("host_fn/serious/") + test.name + "/trace_failed", result.failure);
    check(std::string("host_fn/serious/") + test.name + "/engine_fault",
          result.failure_category == FailureCategory::engine_fault);
    check(std::string("host_fn/serious/") + test.name + "/actions_discarded",
          result.actions.empty() && result.action_rows.empty());
    check(std::string("host_fn/serious/") + test.name + "/fallback_preserved",
          !result.fallback_rows.empty());
  }
}

// P -> C (child) -> G (grandchild), with G also reachable as one of P's
// next_blocks. G's topmost parent is P, so compaction must drop the edge
// instead of turning it into a self-edge.
void test_compact_connections_drops_self_edge() {
  BlockArena arena;
  Block *p = arena.make("call_contract");
  Block *c = arena.make("call_contract");
  Block *g = arena.make("call_contract");
  p->children_blocks.push_back(c);
  c->parent = p;
  c->children_blocks.push_back(g);
  g->parent = c;
  p->next_blocks.push_back(g);

  p->compact_connections();
  bool self_edge = false;
  for (const Block *n : p->next_blocks) {
    self_edge = self_edge || n == p;
  }
  check("block_tree/compact_connections/no_self_edge", !self_edge);
  check("block_tree/compact_connections/grandchild_edge_dropped", p->next_blocks.empty());
}

// X (self-edge) -> Y -> Z, merging {X, Y}. find_frontier walks X's next_blocks,
// so the self-edge would re-queue X forever; the frontier is still {Z}. Reached
// through merge_blocks because find_frontier is private.
void test_find_frontier_terminates_on_self_edge() {
  BlockArena arena;
  Block *x = arena.make("call_contract");
  Block *y = arena.make("call_contract");
  Block *z = arena.make("call_contract");
  y->min_lt_without_initiating_tx = 1;
  z->min_lt_without_initiating_tx = 2;
  x->connect(y);
  y->connect(z);
  x->next_blocks.push_back(x);

  Block *merged = arena.make("merged");
  check("block_tree/find_frontier/merge_accepted", merged->merge_blocks({x, y}));
  check("block_tree/find_frontier/frontier_is_outside_block",
        merged->next_blocks.size() == 1 && merged->next_blocks.front() == z);
  check("block_tree/find_frontier/frontier_reparented", z->previous_block == merged);
}

// A Pred anchor is never prefiltered away and no build fn is registered, so the
// build_fns lookup throws: the shortest route to the failure path.
ClassifyResult classify_with_forced_failure(TraceContext &ctx) {
  std::vector<CompiledMatcher> matchers(1);
  matchers[0].name = "forced-failure";
  matchers[0].anchor_kind = AnchorKind::Pred;
  ClassifySetup setup;
  setup.included = {0};
  FixtureLookupSource src(&ctx.trace.interfaces);
  return classify_trace(ctx, matchers, setup, src);
}

// A single-transaction trace that fails classification still owns its root: the
// basic fallback serializes it, so no unknown action is minted.
void test_childless_root_failure_serializes_root() {
  TraceContext ctx;
  ctx.trace.trace_id = "childless-root-failure";
  auto tx = std::make_unique<Transaction>();
  tx->hash = "childless-root-tx";
  tx->lt = 100;
  tx->now = 200;
  tx->mc_block_seqno = 300;
  tx->account = "0:" + std::string(64, 'B');
  tx->descr = "ord";
  tx->orig_status = "active";
  tx->end_status = "active";
  auto in = std::make_unique<Message>();
  in->msg_hash = "childless-root-message";
  in->tx_hash = tx->hash;
  in->tx_lt = tx->lt;
  in->direction = "in";
  in->source = "0:" + std::string(64, 'A');
  in->destination = tx->account;
  in->opcode = 0x11223344;
  in->value = 1000;
  in->tx = tx.get();
  tx->messages.push_back(std::move(in));
  ctx.trace.transactions.push_back(std::move(tx));
  ctx.tree = to_tree(ctx.trace);
  ctx.root = init_block(ctx.arena, ctx.tree.root);
  check("classify/childless_root_failure/no_children", ctx.tree.root->children.empty());

  ClassifyResult result = classify_with_forced_failure(ctx);
  check("classify/childless_root_failure/failed", result.failure);
  check("classify/childless_root_failure/one_fallback_row", result.fallback_rows.size() == 1);
  check("classify/childless_root_failure/row_is_the_root",
        result.fallback_rows.size() == 1 && result.fallback_rows.front().block != nullptr &&
            result.fallback_rows.front().block->anchor_tx_hash() == "childless-root-tx");
  check("classify/childless_root_failure/no_unknown_trace", !result.unknown_trace);
}

void test_schema_tick_tock() {
  const std::vector<CompiledMatcher> matchers;
  MchEnginePrep prep{&matchers, {}};
  ParsedBlockLookupSource src(nullptr);
  schema::Transaction root{};
  root.hash.set_zero();
  root.account = block::StdAddress(0, root.hash);
  root.description = schema::TransactionDescr_tick_tock{};
  auto lone = classify_schema_trace(prep, "tick-tock", {root}, src);
  check("schema/lone_tick_tock/skipped", lone.outcome == TraceOutcome::skipped_lone_tick_tock);
  check("schema/lone_tick_tock/no_rows", lone.rows.empty());

  schema::Transaction child = root;
  child.hash.data()[31] = 1;
  child.lt = 1;
  child.description = schema::TransactionDescr_ord{};
  schema::Message message{};
  message.hash = child.hash;
  message.source = "0:" + root.hash.to_hex();
  message.destination = message.source;
  child.in_msg = message;
  root.out_msgs.push_back(message);
  auto pair = classify_schema_trace(prep, "tick-tock-child", {root, child}, src);
  check("schema/tick_tock_with_child/not_skipped",
        pair.outcome != TraceOutcome::skipped_lone_tick_tock);
}

void test_schema_history_precedence() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
      "../../../indexer/tests/traces/CFl6PQXQyJ4FQFj9YsZwblnCg3m8Po9wuiBmrR4zGz4=.lz4";
  auto loaded = load_trace(fixture.string());
  check("schema/history/fixture_loaded", loaded.is_ok());
  if (loaded.is_error()) return;
  auto trace = loaded.move_as_ok();
  check("schema/history/fixture_nonempty", !trace.transactions.empty());
  if (trace.transactions.empty()) return;
  auto hash = [](const std::string &encoded) {
    auto bytes = td::base64_decode(encoded).move_as_ok();
    td::Bits256 value;
    value.as_slice().copy_from(td::Slice(bytes));
    return value;
  };
  const std::map<std::string, schema::AccountStatus> statuses{
      {"active", schema::AccountStatus::active}, {"uninit", schema::AccountStatus::uninit},
      {"frozen", schema::AccountStatus::frozen}, {"nonexist", schema::AccountStatus::nonexist}};
  std::vector<schema::Transaction> txs;
  for (const auto &tx : trace.transactions) {
    schema::Transaction st{};
    st.hash = hash(tx->hash);
    st.account = block::StdAddress::parse(td::Slice(tx->account)).move_as_ok();
    st.lt = tx->lt;
    st.now = tx->now;
    st.mc_seqno = tx->mc_block_seqno;
    st.orig_status = statuses.at(tx->orig_status);
    st.end_status = statuses.at(tx->end_status);
    schema::TransactionDescr_ord descr{};
    schema::TrComputePhase_vm compute{};
    compute.exit_code = tx->compute_exit_code.value_or(0);
    descr.compute_ph = compute;
    if (tx->skipped_reason) {
      const std::map<std::string, schema::ComputeSkipReason> reasons{
          {"no_state", schema::cskip_no_state}, {"bad_state", schema::cskip_bad_state},
          {"no_gas", schema::cskip_no_gas}, {"suspended", schema::cskip_suspended}};
      descr.compute_ph = schema::TrComputePhase_skipped{reasons.at(*tx->skipped_reason)};
    }
    descr.aborted = tx->aborted;
    st.description = descr;
    for (const auto &msg : tx->messages) {
      schema::Message sm{};
      sm.hash = hash(msg->msg_hash);
      sm.source = msg->source;
      sm.destination = msg->destination;
      sm.opcode = msg->opcode;
      if (msg->value) sm.value = schema::CurrencyCollection{td::make_refint(*msg->value), {}};
      sm.created_lt = msg->created_lt;
      sm.created_at = msg->created_at;
      sm.bounce = msg->bounce;
      sm.bounced = msg->bounced;
      if (msg->content) sm.body_boc = msg->content->body;
      if (msg->direction == "in") st.in_msg = std::move(sm);
      else st.out_msgs.push_back(std::move(sm));
    }
    txs.push_back(std::move(st));
  }
  auto prepared = make_engine_prep();
  check("schema/history/prepared", prepared.is_ok());
  if (prepared.is_error()) return;
  auto prep = prepared.move_as_ok();
  ParsedBlockLookupSource available(nullptr);
  auto normal = classify_schema_trace(*prep, trace.trace_id, txs, available);
  check("schema/history/classified", normal.outcome == TraceOutcome::classified);
  check("schema/history/normal_rows", !normal.rows.empty());
  ParsedBlockLookupSource unavailable(nullptr,
      [](const std::string &, const std::vector<Value> &) {
        return ParsedBlockLookupSource::Tier2Result{Value::null(), true};
      });
  unavailable.fetch("nominator_pool", {Value::make_str(trace.transactions.front()->account)});
  auto result = classify_schema_trace(*prep, trace.trace_id, txs, unavailable);
  check("schema/history/outcome", result.outcome == TraceOutcome::history_unavailable);
  check("schema/history/rows_retained", !result.rows.empty() && result.rows.size() == normal.rows.size());
}

// Startup is fatal on a skipped matcher, so the compiled-in table must have none:
// a skip means a host name the engine does not provide.
void test_engine_prep_has_no_skips() {
  auto prepared = make_engine_prep();
  if (prepared.is_error()) {
    std::printf("prep error: %s\n", prepared.error().message().c_str());
  }
  check("prep/no_skips", prepared.is_ok());
}

}  // namespace

int run_classify_core_test() {
  g_fail = 0;
  test_host_fn_exception_is_clean_rejection();
  test_host_fn_error_rejects_attempt_and_next_matcher_runs();
  test_host_fn_serious_exception_stays_trace_failure();
  test_compact_connections_drops_self_edge();
  test_find_frontier_terminates_on_self_edge();
  test_childless_root_failure_serializes_root();
  test_schema_tick_tock();
  test_schema_history_precedence();
  test_engine_prep_has_no_skips();
  std::printf("CLASSIFY-CORE-TEST %s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
  return g_fail == 0 ? 0 : 1;
}

}  // namespace mch
