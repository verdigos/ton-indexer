// --shape-test: parity checks for Block::insert_between and the registered
// getgems_proxy_insert shaper. See ShapeTest.h. Mirrors tests/mch_ir/test_shape.py
// (R06/R07): the proxy lands between the parent and the target(s), the target's
// back-edge is reparented, and after the getgems surgery the proxy + sibling
// payout are absorbed into the produced block.
#include "ShapeTest.h"

#include "BlockTree.h"
#include "BuildRuntime.h"
#include "HostRegistry.h"
#include "Value.h"
#include "Walker.h"
#include "host/HostImpls.h"
#include "btypes_gen.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mch {

namespace {

int g_fail = 0;

void check(const std::string &name, bool ok) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
  if (!ok) {
    g_fail++;
  }
}

bool contains(const std::vector<Block *> &v, const Block *b) {
  return std::find(v.begin(), v.end(), b) != v.end();
}

// R06 core mechanic: insert_between must splice the proxy, redirect grandchild
// edges, and reparent the target's back-edge. Mirrors the insert_proxy shaper's
// observable effect in test_ir_engine_invokes_shaper_post_merge.
void test_insert_between() {
  BlockArena arena;
  Block *a = arena.make(mch::btype::kCallContract);
  Block *b = arena.make(mch::btype::kCallContract);
  a->connect(b);  // a.next=[b], b.prev=a

  // a child of `a` that also holds an edge into b (grandchild-rewire path).
  Block *child = arena.make(mch::btype::kCallContract);
  a->children_blocks.push_back(child);
  child->next_blocks.push_back(b);

  // a child of b pointing back at b (reparent path).
  Block *gchild = arena.make(mch::btype::kCallContract);
  b->children_blocks.push_back(gchild);
  gchild->previous_block = b;

  Block *proxy = arena.make(mch::btype::kEmpty);
  a->insert_between({b}, proxy);

  check("insert_between/parent_next_is_proxy", a->next_blocks == std::vector<Block *>{proxy});
  check("insert_between/proxy_next_is_target", proxy->next_blocks == std::vector<Block *>{b});
  check("insert_between/proxy_prev_is_parent", proxy->previous_block == a);
  check("insert_between/target_prev_is_proxy", b->previous_block == proxy);
  check("insert_between/grandchild_edge_redirected",
        child->next_blocks == std::vector<Block *>{proxy});
  check("insert_between/target_backedge_reparented", gchild->previous_block == proxy);
}

// A finish/stop parent holds the produced purchase and captured payout.
// The shaper inserts an empty proxy and absorbs payout plus proxy.
void test_getgems_proxy_insert() {
  BlockArena arena;

  Block *parent = arena.make(mch::btype::kTonTransfer);
  parent->min_lt_without_initiating_tx = 10;
  parent->data = Value::make_dict({{"comment", Value::make_str("finish")}});

  // The built NftPurchaseBlock, already sitting under the finish/stop parent.
  Block *produced = arena.make(mch::btype::kNftPurchase);
  produced->min_lt_without_initiating_tx = 20;
  produced->previous_block = parent;

  Block *payout = arena.make(mch::btype::kTonTransfer);
  payout->min_lt_without_initiating_tx = 30;
  payout->previous_block = parent;

  parent->next_blocks = {produced, payout};

  // The shaper is reached through the registry (proves registration too).
  auto it = host_shapers().find("getgems_proxy_insert");
  check("getgems/shaper_registered", it != host_shapers().end());
  if (it == host_shapers().end()) {
    return;
  }

  ShaperMatch m;
  m.captures["sib_payout"] = payout;
  m.arena = &arena;
  it->second(produced, m);

  // The proxy is absorbed into `produced` (proxy.parent = produced), so
  // compact_connections maps the parent's only edge to `produced`. The payout
  // is also absorbed and is not a sibling of `produced`.
  check("getgems/parent_next_is_produced", parent->next_blocks == std::vector<Block *>{produced});
  check("getgems/parent_dropped_payout", !contains(parent->next_blocks, payout));
  check("getgems/produced_prev_is_parent", produced->previous_block == parent);
  check("getgems/payout_absorbed", contains(produced->children_blocks, payout));
  bool proxy_absorbed = false;
  for (Block *cb : produced->children_blocks) {
    if (cb->btype == mch::btype::kEmpty) {
      proxy_absorbed = true;
      break;
    }
  }
  check("getgems/proxy_absorbed", proxy_absorbed);
}

// Typed lookup keys must not collide across structurally distinct requests.
void test_lookup_key() {
  const std::string embedded_nul("a\0b", 3);
  // Embedded NUL in one string vs two separate strings.
  check("lookup_key/embedded_nul_vs_two_args",
        lookup_key("k", {Value::make_str(embedded_nul)}) !=
            lookup_key("k", {Value::make_str("a"), Value::make_str("b")}));
  // Same textual payload, different Value type (Str vs Account).
  const std::string addr = "0:" + std::string(64, 'A');
  check("lookup_key/str_vs_account",
        lookup_key("k", {Value::make_str(addr)}) !=
            lookup_key("k", {Value::make_account_raw(addr)}));
  // Same textual payload, different Value type (Str vs Bytes).
  check("lookup_key/str_vs_bytes",
        lookup_key("k", {Value::make_str("xy")}) !=
            lookup_key("k", {Value::make_bytes("xy")}));
  // Arity boundary: "kind + one arg" vs "kind-with-suffix + no args" cannot
  // alias, and matching payloads at different arities stay distinct.
  check("lookup_key/arity_distinct",
        lookup_key("k", {Value::make_str("a")}) != lookup_key("k", {}));
  // Kind boundary: a kind whose tail equals the next arg's head must not merge.
  check("lookup_key/kind_arg_boundary",
        lookup_key("ka", {Value::make_str("b")}) !=
            lookup_key("k", {Value::make_str("ab")}));
  // Determinism: identical requests produce identical keys.
  check("lookup_key/deterministic",
        lookup_key("k", {Value::make_str(addr)}) ==
            lookup_key("k", {Value::make_str(addr)}));
}

Block *call(BlockArena &arena, std::uint32_t opcode, std::int64_t min_lt) {
  Block *block = arena.make(mch::btype::kCallContract);
  block->opcode = opcode;
  block->min_lt = min_lt;
  return block;
}

CompiledMatcher peek_matcher(bool consume_again) {
  CompiledMatcher matcher;
  matcher.name = "walker_peek";
  matcher.anchor_kind = AnchorKind::OpcodeSet;
  matcher.anchor_opcodes.insert(1);
  matcher.slot_names = consume_again ? std::vector<std::string>{"root", "seen", "taken"}
                                     : std::vector<std::string>{"root", "seen"};
  matcher.cards.assign(matcher.slot_names.size(), "one");
  matcher.nodes.resize(consume_again ? 3 : 2);

  CompiledNode &root = matcher.nodes[0];
  root.kind = NodeKind::Contract;
  root.opcode = 1;
  root.has_opcode = true;
  root.slot = 0;
  root.child = 1;

  CompiledNode &seen = matcher.nodes[1];
  seen.kind = NodeKind::Contract;
  seen.opcode = 2;
  seen.has_opcode = true;
  seen.slot = 1;
  seen.peek = true;

  if (consume_again) {
    root.children = {2};
    CompiledNode &taken = matcher.nodes[2];
    taken.kind = NodeKind::Contract;
    taken.opcode = 2;
    taken.has_opcode = true;
    taken.slot = 2;
  }
  return matcher;
}

void test_walker_peek_bind_and_aux() {
  BlockArena arena;
  Block *root = call(arena, 1, 10);
  Block *seen = call(arena, 2, 20);
  Block *excess = call(arena, 0xd53276dbu, 30);
  root->connect(seen);
  seen->connect(excess);

  auto result = matcher_match(peek_matcher(false), root, WhereTable{});
  check("walker_peek/matched", result.has_value());
  if (!result) {
    return;
  }
  check("walker_peek/consumes_only_root", result->consumed == std::vector<Block *>{root});
  check("walker_peek/binds_seen",
        result->captures.size() == 2 && result->captures[1].vals == std::vector<Block *>{seen});
  check("walker_peek/skips_aux", !contains(result->consumed, excess));
}

void test_walker_peek_then_consume() {
  BlockArena arena;
  Block *root = call(arena, 1, 10);
  Block *seen = call(arena, 2, 20);
  root->connect(seen);

  auto result = matcher_match(peek_matcher(true), root, WhereTable{});
  check("walker_peek/reuse_matched", result.has_value());
  if (!result) {
    return;
  }
  check("walker_peek/reuse_consumed",
        result->consumed == std::vector<Block *>{root, seen});
  check("walker_peek/reuse_bound_twice",
        result->captures.size() == 3 &&
            result->captures[1].vals == std::vector<Block *>{seen} &&
            result->captures[2].vals == std::vector<Block *>{seen});
}

}  // namespace

int run_shape_test() {
  g_fail = 0;
  test_insert_between();
  test_getgems_proxy_insert();
  test_lookup_key();
  std::printf("SHAPE-TEST %s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
  return g_fail == 0 ? 0 : 1;
}

int run_walker_peek_test() {
  g_fail = 0;
  test_walker_peek_bind_and_aux();
  test_walker_peek_then_consume();
  std::printf("WALKER-PEEK-TEST %s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
  return g_fail == 0 ? 0 : 1;
}

}  // namespace mch
