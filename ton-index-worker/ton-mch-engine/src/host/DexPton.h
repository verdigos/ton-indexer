// Shared TON-proxy master list (STON.fi pTON v1/v2, Tonco v1 wTTON, Tonco v2
// TON wrapper) and wallet-to-asset conversion for DEX hosts.
#pragma once

#include "Value.h"

#include <optional>
#include <string>
#include <vector>

namespace mch {

// STON.fi v2 and Tonco treat wallets for these four TON-proxy masters as TON.
// Matching canonicalizes raw `wc:HEX` addresses, including case.
const std::vector<std::string> &pton_masters();
bool is_pton_master(const std::string &s);

// The normalized wallet.jetton master, or nullopt for a lookup miss or a
// non-string jetton field. pTON conversion and Asset wrapping stay with callers.
std::optional<std::string> wallet_jetton_master_str(const Value &wallet);

// A jetton_wallet lookup's `jetton` master -> Asset, selected by
// `pton_conversion`:
//  - true  (Stonfi v2 / Tonco): a TON-proxy-master wallet becomes Asset(TON); an
//    absent jetton master yields Null (caller treats it as "no asset").
//  - false (Stonfi v1): no TON-proxy conversion (a TON-proxy-master wallet stays
//    a jetton asset); an absent jetton master yields Asset(is_ton=True).
// `wallet` may be Null (a lookup miss): jetton is then absent.
Value wallet_jetton_asset(const Value &wallet, bool pton_conversion);

}  // namespace mch
