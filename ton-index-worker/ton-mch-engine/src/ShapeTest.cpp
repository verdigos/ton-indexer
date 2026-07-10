// --shape-test: parity checks for Block::insert_between and the registered
// getgems_proxy_insert shaper. See ShapeTest.h. Mirrors tests/mch_ir/test_shape.py
// (R06/R07): the proxy lands between the parent and the target(s), the target's
// back-edge is reparented, and after the getgems surgery the proxy + sibling
// payout are absorbed into the produced block.
#include "ShapeTest.h"

#include "BlockTree.h"
#include "HostRegistry.h"
#include "host/HostImpls.h"

#include <algorithm>
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
  Block *a = arena.make("call_contract");
  Block *b = arena.make("call_contract");
  a->connect(b);  // a.next=[b], b.prev=a

  // a child of `a` that also holds an edge into b (grandchild-rewire path).
  Block *child = arena.make("cc");
  a->children_blocks.push_back(child);
  child->next_blocks.push_back(b);

  // a child of b pointing back at b (reparent path).
  Block *gchild = arena.make("gc");
  b->children_blocks.push_back(gchild);
  gchild->previous_block = b;

  Block *proxy = arena.make("empty");
  a->insert_between({b}, proxy);

  check("insert_between/parent_next_is_proxy", a->next_blocks == std::vector<Block *>{proxy});
  check("insert_between/proxy_next_is_target", proxy->next_blocks == std::vector<Block *>{b});
  check("insert_between/proxy_prev_is_parent", proxy->previous_block == a);
  check("insert_between/target_prev_is_proxy", b->previous_block == proxy);
  check("insert_between/grandchild_edge_redirected",
        child->next_blocks == std::vector<Block *>{proxy});
  check("insert_between/target_backedge_reparented", gchild->previous_block == proxy);
}

// R07 analog: the registered getgems_proxy_insert shaper on the finish/stop
// branch. Parent (unconsumed finish ton_transfer) -> [produced, payout]; the
// shaper inserts an empty proxy and absorbs payout+proxy into produced.
void test_getgems_proxy_insert() {
  BlockArena arena;
  const std::string seller = "0:" + std::string(64, 'A');

  Block *parent = arena.make("ton_transfer");
  parent->min_lt_without_initiating_tx = 10;
  parent->data = Value::make_dict({{"comment", Value::make_str("finish")}});

  Block *nft = arena.make("nft_transfer");
  nft->data = Value::make_dict({{"real_prev_owner", Value::make_account_raw(seller)}});

  // The built NftPurchaseBlock, already sitting under the finish/stop parent.
  Block *produced = arena.make("nft_purchase");
  produced->min_lt_without_initiating_tx = 20;
  produced->previous_block = parent;

  Block *payout = arena.make("ton_transfer");
  payout->min_lt_without_initiating_tx = 30;
  payout->previous_block = parent;
  payout->data = Value::make_dict({{"destination", Value::make_account_raw(seller)}});

  parent->next_blocks = {produced, payout};

  // The shaper is reached through the registry (proves registration too).
  auto it = host_shapers().find("getgems_proxy_insert");
  check("getgems/shaper_registered", it != host_shapers().end());
  if (it == host_shapers().end()) {
    return;
  }

  ShaperMatch m;
  m.captures["nft"] = nft;
  m.arena = &arena;
  it->second(produced, m);

  // Post-merge topology (mirrors Python getgems.py + core.py exactly): the
  // proxy is absorbed into `produced` (proxy.parent = produced), so the parent's
  // compact_connections remaps its only edge to the proxy's topmost parent —
  // parent.next collapses to [produced]. Payout is likewise absorbed; the
  // sibling payout no longer hangs off the parent.
  check("getgems/parent_next_is_produced", parent->next_blocks == std::vector<Block *>{produced});
  check("getgems/parent_dropped_payout", !contains(parent->next_blocks, payout));
  check("getgems/produced_prev_is_parent", produced->previous_block == parent);
  check("getgems/payout_absorbed", contains(produced->children_blocks, payout));
  bool proxy_absorbed = false;
  for (Block *cb : produced->children_blocks) {
    if (cb->btype == "empty") {
      proxy_absorbed = true;
      break;
    }
  }
  check("getgems/proxy_absorbed", proxy_absorbed);
}

}  // namespace

int run_shape_test() {
  g_fail = 0;
  test_insert_between();
  test_getgems_proxy_insert();
  std::printf("SHAPE-TEST %s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
  return g_fail == 0 ? 0 : 1;
}

}  // namespace mch
