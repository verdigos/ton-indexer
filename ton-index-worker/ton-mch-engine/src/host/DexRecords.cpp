#include "host/DexRecords.h"

#include "host/HostCommon.h"

#include <utility>

namespace mch {

Value excess_pair(Value asset, Value amount) {
  return Value::make_list(std::vector<Value>{std::move(asset), std::move(amount)});
}

TransferLeg TransferLeg::from_jetton_transfer(const Block *jt) {
  TransferLeg leg;
  leg.asset = data_field(jt, "asset");
  leg.amount = data_field(jt, "amount");
  leg.source = data_field(jt, "sender");
  leg.source_jetton_wallet = data_field(jt, "sender_wallet");
  leg.destination = data_field(jt, "receiver");
  leg.destination_jetton_wallet = data_field(jt, "receiver_wallet");
  return leg;
}

Value TransferLeg::encode() const {
  Value::Fields f;
  f.emplace_back("asset", asset);
  f.emplace_back("amount", amount);
  f.emplace_back("source", source);
  f.emplace_back("source_jetton_wallet", source_jetton_wallet);
  f.emplace_back("destination", destination);
  f.emplace_back("destination_jetton_wallet", destination_jetton_wallet);
  return Value::make_dict(std::move(f));
}

}  // namespace mch
