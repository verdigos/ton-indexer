#include "EmuClassifierBridge.h"

#include "DetectorInterfaceAdapter.h"

#include "TraceEmulator.h"

#include <utility>
#include <variant>
#include <vector>

namespace mch {

TieredLookupSource::InterfaceMap make_interface_map(const ::Trace &trace) {
  TieredLookupSource::InterfaceMap result;

  // Derive NFT code hashes from the detector's account state, preferring the
  // latest emulated state over committed state. Zero means unknown.
  auto account_code_hash = [&trace](const block::StdAddress &addr) {
    td::Bits256 h;
    h.set_zero();
    auto range = trace.emulated_accounts.equal_range(addr);
    if (range.first != range.second) {
      const block::Account &acc = std::prev(range.second)->second;
      if (acc.code.not_null()) {
        return td::Bits256{acc.code->get_hash().bits()};
      }
    }
    auto it = trace.committed_accounts.find(addr);
    if (it != trace.committed_accounts.end() && it->second.code.not_null()) {
      return td::Bits256{it->second.code->get_hash().bits()};
    }
    return h;
  };

  // Use trace-final interfaces so emulated accounts resolve in tier 1. Variants
  // without a tier-1 lookup kind are ignored.
  for (const auto &[addr, ifaces] : trace.interfaces) {
    std::vector<schema::BlockchainInterfaceV2> adapted;
    const td::Bits256 code_hash = account_code_hash(addr);
    for (const auto &iface : ifaces) {
      std::visit([&](const auto &r) {
        if constexpr (requires { to_schema(r); }) {
          auto v = to_schema(r);
          set_nft_code_hash(v, code_hash);
          adapted.push_back(std::move(v));
        }
      }, iface);
    }
    if (!adapted.empty()) {
      result.emplace(addr, std::move(adapted));
    }
  }
  return result;
}

}  // namespace mch
