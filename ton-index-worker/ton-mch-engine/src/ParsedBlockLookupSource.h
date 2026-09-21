// Resolves lookups from interfaces already computed for the block, with an
// injectable fallback for persisted data. Malformed addresses and missing
// accounts are clean misses.
#pragma once

#include "BuildRuntime.h"  // LookupSource, Value

#include "IndexData.h"  // schema::BlockchainInterfaceV2, ParsedBlock (tondb-scanner, PUBLIC)

#include <functional>
#include <map>
#include <string>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mch {

class ParsedBlockLookupSource : public LookupSource {
 public:
  struct Tier2Result {
    Value value;
    bool unavailable{false};
  };
  // Empty hook means no fallback.
  using Tier2Hook = std::function<Tier2Result(const std::string &kind, const std::vector<Value> &args)>;

  using InterfaceMap =
      std::unordered_map<block::StdAddress, std::vector<schema::BlockchainInterfaceV2>>;
  using StableAddressSet = std::unordered_set<block::StdAddress>;

  explicit ParsedBlockLookupSource(const InterfaceMap *account_interfaces, Tier2Hook tier2 = {},
                                   const StableAddressSet *status_stable_accounts = nullptr)
      : account_interfaces_(account_interfaces),
        tier2_(std::move(tier2)),
        status_stable_accounts_(status_stable_accounts) {
  }

  Value fetch(const std::string &kind, const std::vector<Value> &args) const override;

  // Tier-attribution telemetry: every resolved lookup
  // is charged to the tier that served it; unresolved calls are misses. Gives
  // the tier-2 hit rate in a production classifier run
  // (tier2_hits / (tier1_hits + tier2_hits)). Does not affect fetch() results.
  struct LookupStats {
    std::size_t tier1_hits{0};  // served from account_interfaces_ (in-block)
    std::size_t tier2_hits{0};  // served from the tier-2 fallback (DB/kvrocks / fixture)
    // Tier-2 hit attribution per lookup kind. Only served kinds get an entry,
    // and the values sum to `tier2_hits` exactly.
    std::map<std::string, std::size_t> tier2_hits_by_kind;
    std::size_t misses{0};      // null results
    // Miss attribution per lookup kind, so a production run can tell an expected
    // gap (a kind with no tier-1 source) from a real resolution failure. Only
    // missed kinds get an entry, and the values sum to `misses` exactly.
    std::map<std::string, std::size_t> misses_by_kind;
  };
  const LookupStats &stats() const { return stats_; }
  bool history_unavailable() const { return history_unavailable_; }

  // Shared V2-to-Value shape map for tier 1 and tier 2.
  // Null == this variant does not answer this kind. A tier-2 hook that produces
  // a schema::*V2 renders it through HERE rather than building a Value of its
  // own. This makes the two tiers shape-identical by construction
  // instead of by review.
  static Value iface_value(const std::string &kind, const schema::BlockchainInterfaceV2 &iface);

 private:
  // Tier-1 resolve from account_interfaces_. nullopt means no map entry; an
  // engaged null Value means the entry has no matching kind.
  std::optional<Value> tier1(const std::string &kind, const std::string &addr) const;

  const InterfaceMap *account_interfaces_;
  Tier2Hook tier2_;
  const StableAddressSet *status_stable_accounts_;
  mutable bool history_unavailable_{false};
  mutable LookupStats stats_;  // fetch() is const; counters are telemetry
};

}  // namespace mch
