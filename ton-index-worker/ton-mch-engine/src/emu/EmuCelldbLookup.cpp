#include "EmuCelldbLookup.h"

#include "EmuInterfaces.h"
#include "EmuJvaultChain.h"
#include "EmuTypes.h"  // emu_now_us

#include "smc-interfaces/NominatorPool.h"
#include "smc-interfaces/Tokens.h"

#include <utility>

namespace mch {

namespace {

// Memo key. block::StdAddress::parse already folded case and encoding, so the
// canonical raw form is the identity; the NUL keeps a kind whose tail matches an
// address head from aliasing (same argument as lookup_key's framing).
std::string memo_key(const std::string &kind, const block::StdAddress &addr) {
  return kind + std::string(1, '\0') + std::to_string(addr.workchain) + ":" + addr.addr.to_hex();
}

}  // namespace

EmuCelldbTier2::EmuCelldbTier2(const AllShardStates *shard_states,
                               std::shared_ptr<block::ConfigInfo> config, Tier2Budget budget)
    : shard_states_(shard_states)
    , config_(std::move(config))
    , budget_(budget)
    , deadline_us_(emu_now_us() + budget.wall_us)
    // Captures the states pointer, NOT `this`: the default source must survive
    // a move of the object that installed it.
    , account_source_([states = shard_states](const block::StdAddress &a) {
      return lookup_account(*states, a);
    }) {
}

const schema::AccountState *EmuCelldbTier2::account(const block::StdAddress &addr) {
  auto it = account_memo_.find(addr);
  if (it != account_memo_.end()) {
    return it->second ? &*it->second : nullptr;
  }
  // The wall window is half-open [start, deadline): reaching the deadline
  // exhausts it. Strict `>` would let a wall_us of 0 serve every read that
  // lands inside the same microsecond emu_now_us() truncates to.
  if (stats_.fetched >= budget_.fetches || emu_now_us() >= deadline_us_) {
    stats_.budget_exhausted++;
    // NOT memoized: the budget is a property of the run, not of the account.
    return nullptr;
  }
  stats_.fetched++;
  auto r = account_source_(addr);
  auto [pos, _] = account_memo_.emplace(
      addr, r.is_ok() ? std::optional<schema::AccountState>(r.move_as_ok()) : std::nullopt);
  return pos->second ? &*pos->second : nullptr;
}

Value EmuCelldbTier2::resolve(const std::string &kind, const block::StdAddress &addr) {
  // jvault is three raw data-cell reads and no detector at all, so it does not
  // go through the V2 render below.
  if (kind == "jvault_assets") {
    return jvault_assets(addr);
  }
  // Kind gate BEFORE the read, so an unserved kind never spends budget. This is
  // what keeps nft_item / nft_sale / nft_auction — tier-1-only this wave — from
  // costing a celldb round each.
  if (kind != "jetton_wallet" && kind != "dedust_pool" && kind != "nominator_pool") {
    return Value::null();
  }

  const schema::AccountState *st = account(addr);
  if (st == nullptr) {
    return Value::null();
  }

  // Each arm: run the SHARED tondb-scanner detector, adapt its Result to the
  // pipeline's V2 struct, and hand that to tier 1's own renderer.
  // The config check guards the two arms that run a get-method —
  // execute_smc_method dereferences the config unguarded. nominator_pool and
  // jvault never reach it (a code-hash predicate and three cell parses).
  std::optional<schema::BlockchainInterfaceV2> iface;
  if (kind == "jetton_wallet") {
    if (config_ == nullptr) {
      return Value::null();
    }
    auto r = JettonWalletDetectorR::detect(addr, st->code, st->data, *shard_states_, config_);
    if (r.is_ok()) {
      iface = to_v2(r.move_as_ok());
    }
  } else if (kind == "dedust_pool") {
    if (config_ == nullptr) {
      return Value::null();
    }
    auto r = DedustPoolDetector::detect(addr, st->code, st->data, *shard_states_, config_);
    if (r.is_ok()) {
      iface = to_v2(r.move_as_ok());
    }
  } else if (kind == "nominator_pool") {
    auto r = NominatorPoolContract::detect(addr, st->code, st->data);
    if (r.is_ok()) {
      iface = to_v2(r.move_as_ok());
    }
  }
  // nft_item / nft_sale / nft_auction stay tier-1-only this wave (see the kind
  // gate above): their detectors carry the DNS/auction time-branching that is
  // excluded from any identity assertion, and the measured recovery target for
  // this wave is jvault + dedust.
  if (!iface) {
    return Value::null();
  }
  return ParsedBlockLookupSource::iface_value(kind, *iface);
}

Value EmuCelldbTier2::jvault_assets(const block::StdAddress &stake_wallet) {
  const schema::AccountState *wallet = account(stake_wallet);
  if (wallet == nullptr) {
    return Value::null();
  }
  auto r_wallet = parse_jvault_stake_wallet(wallet->data);
  if (r_wallet.is_error()) {
    return Value::null();
  }
  auto parts = r_wallet.move_as_ok();
  Value jvault_asset = jvault_asset_of(parts.minter);

  auto r_pool_addr = block::StdAddress::parse(td::Slice(parts.staking_pool_raw));
  if (r_pool_addr.is_error()) {
    return Value::null();
  }
  const schema::AccountState *pool = account(r_pool_addr.move_as_ok());
  if (pool == nullptr) {
    // THE PARTIAL CASE, and it is semantic rather than an error: Python returns
    // (staking_pool, None, jvault_asset) when the pool's extra data is missing.
    return jvault_record(parts.staking_pool, Value::null(), std::move(jvault_asset));
  }

  auto r_lock = parse_jvault_pool_lock_wallet(pool->data);
  if (r_lock.is_error()) {
    // Python's KeyError on ['lock_wallet_address'] -> outer except -> None.
    return Value::null();
  }
  // Through fetch(), so the lock wallet shares the memo and the budget. It is
  // out-of-trace by construction, so routing it past tier 1 costs nothing.
  Value jw = fetch("jetton_wallet", {Value::make_str(r_lock.move_as_ok())});
  const Value *jetton = jw.field("jetton");
  if (jetton == nullptr || jetton->t != VType::Str) {
    return Value::null();  // get_jetton_wallet None -> .jetton raises -> None
  }
  auto jn = normalize_raw_address(jetton->str);
  return jvault_record(parts.staking_pool, Value::make_asset_jetton(jn ? *jn : jetton->str),
                       std::move(jvault_asset));
}

Value EmuCelldbTier2::fetch(const std::string &kind, const std::vector<Value> &args) {
  if (shard_states_ == nullptr || shard_states_->empty()) {
    return Value::null();  // listener-emulated trace: no states to read
  }
  // Same one-Str-argument guard the fixture source and tier 1 apply.
  if (args.size() != 1 || args[0].t != VType::Str) {
    return Value::null();
  }
  auto r_addr = block::StdAddress::parse(td::Slice(args[0].str));
  if (r_addr.is_error()) {
    return Value::null();
  }
  block::StdAddress addr = r_addr.move_as_ok();

  const std::string key = memo_key(kind, addr);
  auto it = value_memo_.find(key);
  if (it != value_memo_.end()) {
    stats_.memo_hits++;
    return it->second;
  }
  // The TLB parsers throw vm::VmError, which does NOT derive from std::exception
  // and which classify_txs' catch-all would turn into an engine_fault — i.e. one
  // malformed account data cell somewhere in celldb would demote a whole trace to
  // the basic-actions fallback. A tier-2 read that cannot be parsed is a MISS.
  Value v;
  try {
    v = resolve(kind, addr);
  } catch (...) {
    v = Value::null();
  }
  // Misses are memoized too: a null is a real answer, and the fixpoint would
  // otherwise pay for the same failed detection once per round.
  value_memo_.emplace(key, v);
  return v;
}

}  // namespace mch
