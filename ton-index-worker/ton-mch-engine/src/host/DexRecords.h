// Transfer legs and excess refunds shared by DEX hosts.
#pragma once

#include "Value.h"

namespace mch {

struct Block;  // BlockTree.h

// An excess refund represented as the shared [asset, amount] pair.
Value excess_pair(Value asset, Value amount);

// A full transfer leg {asset, amount, source, source_jetton_wallet,
// destination, destination_jetton_wallet} (the DEX incoming/outgoing transfer).
struct TransferLeg {
  Value asset;
  Value amount;
  Value source;
  Value source_jetton_wallet;
  Value destination;
  Value destination_jetton_wallet;
  Value encode() const;  // Dict, 6 fields in the order above

  // Read the six fields out of a produced `jetton_transfer` block's data dict
  // (asset/amount/sender/sender_wallet/receiver/receiver_wallet). The shared
  // jetton-leg shape used by the Stonfi cores.
  static TransferLeg from_jetton_transfer(const Block *jt);
};

}  // namespace mch
