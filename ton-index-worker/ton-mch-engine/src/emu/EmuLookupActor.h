// Tier-2 lookup transport (em2-tier2-design §5.3/§6.5, W3).
//
// One actor owning ONE `sw::redis::Redis`, issuing BLOCKING MGETs in its actor
// body. Async redis++ is not an option — the async sources are gated behind
// REDIS_PLUS_PLUS_BUILD_ASYNC, which is unset and would pull in libuv — and
// blocking-inside-an-actor is the emulator's own precedent (RedisInsertManager
// spawns a TraceInserter per trace doing synchronous hlen/hget/exec). The cost
// is one of the scheduler's threads for the duration of a round trip, which is
// why W4 runs a POOL of these and suspends the classifier rather than blocking
// it.
//
// All decoding lives in KvrocksDecode (mch-classify, no redis dependency); this
// TU only moves bytes. That split is what lets the mapping be gated with no
// store — see `--kvrocks-decode-test`.
#pragma once

#include "ClassifyDriver.h"  // LookupWants
#include "EmuTypes.h"        // EmuTier2Fetch
#include "KvrocksDecode.h"   // Value, PayloadFetch

#include "KvrocksState.h"  // KvrocksConfig / KvrocksClient (+ sw::redis)

#include "td/actor/actor.h"

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mch {

// §5.3: the KvrocksConfig DEFAULTS are a hazard on this path and must be
// overridden explicitly.
//   - connect 10 s (KvrocksState.h:37) would stall a NON-PREEMPTIBLE actor body
//     for ten seconds per reconnect against a down store;
//   - a pool of 1 with wait_timeout 0 parks callers on a condvar with NO
//     timeout (connection_pool.cpp:230-233), i.e. a deadlock rather than an
//     error, which is why each actor gets its own Redis and a bounded wait.
inline constexpr int kTier2ConnectMs = 150;
inline constexpr int kTier2SocketMs = 150;
inline constexpr int kTier2PoolWaitMs = 50;
inline constexpr std::size_t kTier2PoolSize = 2;

// The caller's config (uri / sentinels / credentials / db) with those four
// pinned. Keeping the rest of KvrocksConfig means Sentinel support and the
// connection building come from the module that already owns them.
kvrocks_state::KvrocksConfig tier2_kvrocks_config(kvrocks_state::KvrocksConfig cfg);

// §5.3: a POOL, because each actor blocks one of the scheduler's threads for
// the duration of a round trip. Round-robined, so one slow store round cannot
// serialize every suspended trace behind it.
inline constexpr std::size_t kLookupActors = 4;

struct EmuLookupStats {
  std::size_t batches{0}, keys{0}, fetch_errors{0};
  std::size_t fetch_us_total{0}, fetch_us_max{0};
};

class EmuLookupActor : public td::actor::Actor {
 public:
  explicit EmuLookupActor(kvrocks_state::KvrocksConfig cfg)
      : cfg_(tier2_kvrocks_config(std::move(cfg))) {
  }

  // Resolve one collect-pass batch. ALWAYS answers, never throws.
  //
  //  - success => an entry per requested key, a miss as an explicit null, so
  //    the caller's fixpoint can tell "fetched, absent" from "not fetched" and
  //    cannot spin on a permanently-missing key;
  //  - TRANSPORT failure => an ERROR promise. The classifier answers it exactly
  //    as it answers an all-null batch (§8.1 — infrastructure problems degrade
  //    classification, they do not fail it), and additionally counts it against
  //    the circuit breaker (§5.6). W3 answered these all-null instead, which
  //    left the breaker with no signal to open on and on_lookup_failed dead;
  //    the error is what separates "the store said no" from "the store is gone"
  //    without changing a single classified byte.
  void fetch_batch(LookupWants wanted, td::Promise<std::map<std::string, Value>> promise);

  const EmuLookupStats &stats() const { return stats_; }

 private:
  void start_up() override;
  // One MGET for the round's keys. Absent keys come back as empty strings, so
  // the resolver can tell "fetched, absent" from "not fetched".
  std::map<std::string, std::string> mget(const std::vector<std::string> &keys);

  kvrocks_state::KvrocksConfig cfg_;
  std::unique_ptr<kvrocks_state::KvrocksClient> client_;
  std::shared_ptr<sw::redis::Redis> redis_;  // null until a connection succeeds
  EmuLookupStats stats_;
};

// The classifier-side adapter: an EmuTier2Fetch that round-robins batches over
// `pool`. The pool's actors must outlive the classifier. This is the ONLY place
// that knows the fetch is served by actors — the classifier itself holds a
// plain functor, which is what lets the suspension loop be gated with no store
// and no redis++ (--tier2-suspend-test).
EmuTier2Fetch lookup_pool_fetch(std::vector<td::actor::ActorId<EmuLookupActor>> pool);

}  // namespace mch
