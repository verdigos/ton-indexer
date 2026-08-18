#include "host/HostImpls.h"

#include "BlockTree.h"
#include "HostRegistry.h"
#include "btypes_gen.h"

namespace mch {

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
