#include "EmuLookupActor.h"

#include "EmuTypes.h"  // emu_now_us

#include "td/utils/logging.h"

#include <chrono>
#include <exception>
#include <iterator>
#include <utility>

namespace mch {

kvrocks_state::KvrocksConfig tier2_kvrocks_config(kvrocks_state::KvrocksConfig cfg) {
  cfg.connect_timeout = std::chrono::milliseconds(kTier2ConnectMs);
  cfg.socket_timeout = std::chrono::milliseconds(kTier2SocketMs);
  cfg.wait_timeout = std::chrono::milliseconds(kTier2PoolWaitMs);
  if (cfg.pool_size < kTier2PoolSize) {
    cfg.pool_size = kTier2PoolSize;
  }
  return cfg;
}

void EmuLookupActor::start_up() {
  // Connecting here rather than in the constructor keeps a store that is down
  // at startup from taking the emulator with it: the first fetch_batch retries,
  // and until then every lookup is simply a null.
  try {
    client_ = std::make_unique<kvrocks_state::KvrocksClient>(cfg_);
    redis_ = client_->make_redis();
  } catch (const std::exception &e) {
    LOG(WARNING) << "[mch-emu] tier-2 connect failed: " << e.what();
    redis_.reset();
  }
}

std::map<std::string, std::string> EmuLookupActor::mget(const std::vector<std::string> &keys) {
  std::map<std::string, std::string> out;
  if (keys.empty()) {
    return out;
  }
  // Every key is a full payload key, so ONE MGET serves a round even when it
  // spans tables — fewer round trips than the per-table pipeline §6.5 sketches,
  // same result.
  if (redis_ == nullptr) {
    try {
      client_ = std::make_unique<kvrocks_state::KvrocksClient>(cfg_);
      redis_ = client_->make_redis();
    } catch (const std::exception &e) {
      stats_.fetch_errors++;
      LOG(WARNING) << "[mch-emu] tier-2 reconnect failed: " << e.what();
      return out;  // all keys unanswered -> the resolver makes them nulls
    }
  }
  const std::int64_t started_us = emu_now_us();
  try {
    std::vector<sw::redis::OptionalString> values;
    values.reserve(keys.size());
    redis_->mget(keys.begin(), keys.end(), std::back_inserter(values));
    for (std::size_t i = 0; i < keys.size() && i < values.size(); i++) {
      out.emplace(keys[i], values[i] ? *values[i] : std::string());
    }
  } catch (const std::exception &e) {
    stats_.fetch_errors++;
    // Drop the connection so the next batch reconnects instead of reusing a
    // socket the store may have closed under us.
    redis_.reset();
    LOG(WARNING) << "[mch-emu] tier-2 MGET failed (" << keys.size() << " keys): " << e.what();
    return {};
  }
  const std::int64_t elapsed = emu_now_us() - started_us;
  if (elapsed > 0) {
    auto us = static_cast<std::size_t>(elapsed);
    stats_.fetch_us_total += us;
    if (us > stats_.fetch_us_max) {
      stats_.fetch_us_max = us;
    }
  }
  stats_.keys += keys.size();
  return out;
}

void EmuLookupActor::fetch_batch(LookupWants wanted,
                                 td::Promise<std::map<std::string, Value>> promise) {
  stats_.batches++;
  // mget() counts every connect/MGET failure, so the delta over this batch is
  // exactly "the transport failed somewhere in here" — including inside the
  // chained jvault rounds, where a later failure already means a null record.
  const std::size_t errors_before = stats_.fetch_errors;
  std::map<std::string, Value> out;
  try {
    out = kvrocks_resolve_batch(wanted, [this](const std::vector<std::string> &keys) {
      return mget(keys);
    });
  } catch (const std::exception &e) {
    // Decoding is written not to throw, so this is belt and braces — but an
    // unanswered promise would wedge a suspended trace forever (§5.5), which is
    // a far worse failure than a degraded classification.
    stats_.fetch_errors++;
    LOG(WARNING) << "[mch-emu] tier-2 batch failed: " << e.what();
    out.clear();
  }
  if (stats_.fetch_errors != errors_before) {
    // The caller degrades this to all-null exactly as §8.1 requires; the error
    // carries the one bit all-null cannot — that the STORE failed rather than
    // the keys being absent — which is what the breaker opens on (§5.6).
    promise.set_error(td::Status::Error("tier-2 fetch failed"));
    return;
  }
  // The resolver already answers every key it accepted; anything it skipped
  // becomes an explicit null here, so the caller's fixpoint can never
  // re-request it.
  for (const auto &[key, ka] : wanted) {
    out.emplace(key, Value::null());
  }
  promise.set_value(std::move(out));
}

EmuTier2Fetch lookup_pool_fetch(std::vector<td::actor::ActorId<EmuLookupActor>> pool) {
  if (pool.empty()) {
    return {};  // no pool == tier 2 off; the classifier never suspends
  }
  // Shared, not per-copy: std::function copies would each restart at 0 and pin
  // every batch on the first actor.
  auto next = std::make_shared<std::size_t>(0);
  return [pool = std::move(pool), next](LookupWants wanted,
                                        td::Promise<std::map<std::string, Value>> promise) {
    const std::size_t i = (*next)++ % pool.size();
    td::actor::send_closure(pool[i], &EmuLookupActor::fetch_batch, std::move(wanted),
                            std::move(promise));
  };
}

}  // namespace mch
