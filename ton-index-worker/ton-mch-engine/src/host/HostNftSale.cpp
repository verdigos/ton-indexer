// GetGems sale/auction family: the deployment predicate and the finish/stop
// sibling-payout shaper.
#include "host/HostImpls.h"

#include "BlockTree.h"
#include "HostRegistry.h"
#include "btypes_gen.h"

namespace mch {

namespace {

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
