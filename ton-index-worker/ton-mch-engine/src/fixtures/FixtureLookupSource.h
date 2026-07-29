// TEST-ONLY. Part of mch-fixtures; never linked into mch-classify or any
// product binary. See ton-mch-engine/CMakeLists.txt MCH_FIXTURE_SOURCES.
//
// Offline LookupSource over a fixture's `interfaces` section (Trace.interfaces
// — the data Python's TestInterfaceRepository serves). Kind semantics mirror
// the REAL registered Python lookup fns:
//  - jetton_wallet (registry.default_lookups): key = arg.upper();
//    account["JettonWallet"] -> Obj{balance, address, owner, jetton}; else null.
//  - nft_item (builders/dns.py RAW variant — the artifact's only consumer):
//    key = arg UN-normalized; account["NftItem"] -> Obj{address, init, index,
//    collection_address, owner_address, content}; else null.
//  - nominator_pool (builders/nominator.py): key = arg raw; the account's
//    whole interfaces Dict when it contains "NominatorPool", else null.
// Deviation note: a non-Str argument yields null here where Python's
// jetton_wallet would raise (str.upper on a non-str -> EvalError -> same
// rejection only when the spec null-checks); real specs pass envelope address
// strings only.
//
// The kind SET is not test-only — runnability gating (BuildDriver) and the
// production ParsedBlockLookupSource both key off it, so it lives lib-side as
// lookup_kinds() (BuildRuntime.h).
#pragma once

#include "BuildRuntime.h"  // LookupSource, lookup_kinds
#include "Value.h"

#include <map>
#include <string>
#include <vector>

namespace mch {

class FixtureLookupSource : public LookupSource {
 public:
  explicit FixtureLookupSource(const std::map<std::string, Value> *interfaces)
      : interfaces_(interfaces) {
  }
  Value fetch(const std::string &kind, const std::vector<Value> &args) const override;

 private:
  const std::map<std::string, Value> *interfaces_;
};

}  // namespace mch
