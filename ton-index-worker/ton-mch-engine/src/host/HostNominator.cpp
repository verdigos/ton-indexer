// Nominator host predicate (builders/nominator.py). See host/HostImpls.h for
// the internal registry surface and HostRegistry.h for the public one.
#include "host/HostImpls.h"

#include "BlockTree.h"

namespace mch {

// builders/nominator.py nominator_pool_withdraw_parent: the payout's
// previous_block is the pool's ProcessWithdrawRequests call (opcode 0x2).
// isinstance(parent, CallContractBlock) == btype "call_contract" (composed
// blocks never carry that btype).
bool nominator_pool_withdraw_parent(const Block *b) {
  const Block *p = b->previous_block;
  return p != nullptr && p->btype == "call_contract" && p->opcode && *p->opcode == 0x00000002;
}

}  // namespace mch
