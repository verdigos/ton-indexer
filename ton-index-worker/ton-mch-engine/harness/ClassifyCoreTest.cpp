// --classify-core-test: exercises the compact_connections / find_frontier
// topology guards and the childless-root branch of the classify failure path.
// No fixture corpus, database or node is needed.
#include "ClassifyCoreTest.h"

#include "BlockTree.h"
#include "ClassifyCore.h"
#include "IrTables.h"
#include "TraceLoader.h"
#include "fixtures/FixtureLookupSource.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace mch {
namespace {

int g_fail = 0;

void check(const std::string &name, bool ok) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
  if (!ok) g_fail++;
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

}  // namespace

int run_classify_core_test() {
  g_fail = 0;
  test_compact_connections_drops_self_edge();
  test_find_frontier_terminates_on_self_edge();
  test_childless_root_failure_serializes_root();
  std::printf("CLASSIFY-CORE-TEST %s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
  return g_fail == 0 ? 0 : 1;
}

}  // namespace mch
