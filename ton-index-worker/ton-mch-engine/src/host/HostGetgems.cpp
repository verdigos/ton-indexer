// Getgems host shaper (builders/getgems.py getgems_proxy_insert). See
// host/HostImpls.h for the internal registry surface and HostRegistry.h for the
// public one.
//
// STATUS: only the `shape` binding is landed here — the getgems match-time
// predicates are deliberately NOT ported yet (they gate a builder-tier matcher
// that cannot run until the Python declarative/hybrid conversion; port each
// pred with that conversion, not speculatively). The shaper + Block::insert_between
// are the reusable substrate the conversion (and stonfi_v2_provide) will need,
// so they land now and are gated by the --shape-test harness, not by classify
// (no classify matcher uses a shaper today; getgems stays pred-SKIP).
//
// FIDELITY NOTE: the finish/stop branch keys on the parent ton_transfer's
// `comment`, which the C++ leaf_data subset does NOT populate today (the
// documented "wire through when first needed" gap). The shaper reads it from
// block.data and degrades to a no-op when absent (byte-safe); the --shape-test
// harness sets the comment on its synthetic blocks to drive the finish/stop path.
#include "host/HostImpls.h"

#include "BlockTree.h"
#include "HostRegistry.h"

namespace mch {

namespace {

// Two Account values are equal iff same addr_none state and canonical string
// (mirrors AccountId.__eq__ over the canonical "wc:HEX" form).
bool same_account(const Value &a, const Value &b) {
  return a.t == VType::Account && b.t == VType::Account && a.addr_none == b.addr_none &&
         a.str == b.str;
}

}  // namespace

// builders/getgems.py getgems_proxy_insert shaper: finish/stop sale-contract
// branch only. Verbatim legacy surgery — insert an EmptyBlock proxy between the
// unconsumed finish/stop parent and [produced, payout], then absorb the sibling
// payout into the produced block (it was never pattern-consumed).
void getgems_proxy_insert(Block *produced, const ShaperMatch &m) {
  Block *nft = m.capture("nft");
  if (nft == nullptr || produced == nullptr) {
    return;
  }
  Block *parent = produced->previous_block;  // finish/stop parent (post-merge)
  if (parent == nullptr || parent->btype != "ton_transfer") {
    return;  // normal branch: no proxy, no surgery
  }
  const Value *cmt = parent->data.field("comment");
  if (cmt == nullptr || cmt->t != VType::Str || (cmt->str != "finish" && cmt->str != "stop")) {
    return;
  }
  const Value *real_prev_owner = nft->data.field("real_prev_owner");
  if (real_prev_owner == nullptr || real_prev_owner->is_null()) {
    return;
  }
  Block *payout = nullptr;
  for (Block *n : parent->next_blocks) {
    if (n == produced || n->btype != "ton_transfer") {
      continue;
    }
    const Value *dest = n->data.field("destination");
    if (dest != nullptr && same_account(*dest, *real_prev_owner)) {
      payout = n;
      break;
    }
  }
  if (payout == nullptr || m.arena == nullptr) {
    return;
  }
  Block *proxy = m.arena->make("empty");
  parent->insert_between({produced, payout}, proxy);
  produced->merge_blocks({payout, proxy});
  produced->compact_connections();
}

}  // namespace mch
