#include "EmuClassifierActor.h"

#include "BlockTree.h"
#include "ClassifyDriver.h"
#include "EmuActionSerialize.h"
#include "EmuCapture.h"
#include "EmuTraceConvert.h"
#include "SchemaTraceLoader.h"

#include "td/utils/logging.h"
#include "td/utils/port/path.h"

#include <algorithm>
#include <exception>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mch {

namespace {

constexpr double kStatsIntervalSec = 10.0;
constexpr std::size_t kMaxSeenTraces = 65536;

const char *category_name(FailureCategory c) {
  switch (c) {
    case FailureCategory::none: return "none";
    case FailureCategory::lookup_infra_fail: return "lookup_infra_fail";
    case FailureCategory::engine_fault: return "engine_fault";
    case FailureCategory::malformed_trace: return "malformed_trace";
  }
  return "?";
}

// Value is not unconditionally self-contained (Value.h:47-60): VType::Block
// holds a RAW arena Block* that dies with the TraceContext this call owns.
// ActionBuild's fills go through av_addr/av_amount/dg, so a Block-typed value
// reaching an Action is believed impossible — this turns "believed impossible"
// into a counter instead of a use-after-free at the integration point.
// VType::Cell is deliberately left alone: those stay valid for as long as the
// view's anchor lives, and since W3 serializes inside this call, that is the
// whole window in which anything reads them.
// NOT a pure observation: Value's `items`/`fields` payloads are shared_ptrs
// shared with the arena-side structures the fills read from, so nulling a
// Block-typed value here mutates that shared structure too (idempotent, and
// the arena dies at the end of this call either way) — which is why
// arena_refs_scrubbed counts a repair, not a sighting.
std::size_t scrub_value(Value &v) {
  if (v.t == VType::Block) {
    v = Value::null();
    return 1;
  }
  std::size_t n = 0;
  // The composite *_data columns are Dict/Obj values, so the walk has to go
  // through containers — a top-level-only pass would see nothing.
  if (v.items) {
    for (Value &item : *v.items) {
      n += scrub_value(item);
    }
  }
  if (v.fields) {
    for (auto &[key, field] : *v.fields) {
      n += scrub_value(field);
    }
  }
  return n;
}

std::size_t scrub_arena_refs(Action &a) {
  std::size_t n = 0;
  for (Value *v : {&a.source, &a.source_secondary, &a.destination, &a.destination_secondary,
                   &a.asset, &a.asset_secondary, &a.asset2, &a.asset2_secondary, &a.amount,
                   &a.value, &a.opcode, &a.ton_transfer_data, &a.jetton_transfer_data,
                   &a.jetton_swap_data, &a.nft_transfer_data, &a.nft_listing_data,
                   &a.dex_deposit_liquidity_data, &a.dex_withdraw_liquidity_data, &a.staking_data,
                   &a.evaa_withdraw_data, &a.vesting_send_message_data, &a.layerzero_packet_data,
                   &a.jvault_stake_data, &a.jvault_claim_data, &a.change_dns_record_data,
                   &a.extra}) {
    n += scrub_value(*v);
  }
  return n;
}

// The COMMITTED pass -> Action rows. Same sequence and the same FailureCategory
// mapping as the EM1.5 inline path, with build_action run INSIDE the try —
// while the arena the CoreActions point into (owned by dr.ctx) is still alive.
// Never throws.
//
// ONE pass reaches this function, ever: the fixpoint discards every superseded
// round's context and result internally, so `rows` cannot accumulate across
// rounds and only the committed pass is serialized (em2-writeback-design §3's
// S14 constraint, satisfied by construction rather than by clearing).
void rows_from_pass(DriverResult &dr, EmuClassifyResult &res, std::vector<Action> &rows,
                    std::vector<std::string> &matcher_names, std::size_t &scrubbed,
                    EmuTier2Stats &t2) {
  try {
    if (dr.malformed) {
      res.failure = true;
      res.failure_reason = dr.error;
      res.failure_category = FailureCategory::malformed_trace;
      return;
    }
    // Fixpoint telemetry (§8.4). `rounds` stays 1 per emission while no
    // Tier2Fetch is configured — the loop commits round 0 when nothing can be
    // fetched — so a rounds total above `emissions` is the first sign tier 2 is
    // live and re-classifying.
    t2.rounds += dr.rounds;
    if (dr.rounds_exhausted) {
      t2.rounds_exhausted++;
    }
    t2.requested += dr.requested.size();
    for (const auto &[key, ka] : dr.requested) {
      t2.requested_by_kind[ka.first]++;
    }
    const ClassifyResult &cres = dr.res;
    res.failure = cres.failure;
    res.failure_reason = cres.failure_reason;
    res.failure_category = cres.failure_category;
    res.lookup_stats = dr.lookup_stats;
    // §8.3: the committed pass's action-scoped wrong-value rejects. Loud on
    // purpose — each one is an action that was DROPPED rather than published
    // wrong, and the site tells an operator which matcher and which lookup.
    if (cres.wrong_value_guard > 0) {
      t2.wrong_value_guard += cres.wrong_value_guard;
      std::string sites;
      for (const auto &[site, n] : cres.wrong_value_sites) {
        if (!sites.empty()) sites += ",";
        sites += site + "=" + std::to_string(n);
      }
      LOG(DEBUG) << "[mch-emu] trace=" << res.trace_id << " wrong_value_guard="
                 << cres.wrong_value_guard << " sites{" << sites << "}";
    }

    // On failure the regular actions are discarded and the basic-actions
    // fallback rows are what production commits (Classify.cpp:186-192).
    res.used_fallback = cres.failure;
    const std::vector<CoreAction> &core =
        cres.failure ? cres.fallback_actions : cres.actions;
    std::set<std::string> names;
    for (const CoreAction &a : core) {
      Action act;
      if (!build_action(a.produced, act)) {  // btype outside the ported fill set
        res.unported_btypes++;
        continue;
      }
      scrubbed += scrub_arena_refs(act);
      rows.push_back(std::move(act));
      names.insert(a.matcher_name);
    }
    matcher_names.assign(names.begin(), names.end());
    // W4's synthetic row, on the emulated path: classification succeeded and
    // matched nothing, so the trace ships "we looked and found nothing" instead
    // of an empty array that ton-index-go reports as a successful
    // classification (§6, §7). dr.ctx->trace carries the aggregates
    // schema_to_trace derived — the emulator has no trace-level lt/utime of its
    // own, and neither does Python here (event_deserializer.py:146-151 takes
    // max over the transactions too), so the two agree by construction.
    if (cres.unknown_trace) {
      rows.push_back(create_unknown_action(dr.ctx->trace));
    }
  } catch (const std::exception &e) {
    res.failure = true;
    res.failure_reason = std::string("exception: ") + e.what();
    res.failure_category = FailureCategory::engine_fault;
  } catch (...) {
    res.failure = true;
    res.failure_reason = "unknown exception";
    res.failure_category = FailureCategory::engine_fault;
  }
}

// "wc:HEXUPPER" — the canonical raw form AccountId.as_str() produces and the
// one the engine puts in Action::accounts, so the _aai keys and the
// referenced_accounts payload join against the same strings the actions carry.
std::string addr_str(const block::StdAddress &a) {
  return std::to_string(a.workchain) + ":" + a.addr.to_hex();
}

// Everything the MULTI needs, built here (§3) while the view is still alive.
// The single serialization call site: the tier-2 wave's suspension loop re-wraps
// the call to this function, not the writer.
EmuActionPayload build_payload(const EmuTraceView &view, const std::vector<Action> &rows,
                               std::int64_t aai_score, ActionSerializeStats &st) {
  EmuActionPayload payload;
  payload.action_count = rows.size();
  payload.aai_score = aai_score;
  payload.actions_blob = serialize_actions(rows, view, &st);

  std::set<std::string> tx_accounts;
  for (const EmuTxRef &node : view.nodes) {
    tx_accounts.insert(addr_str(node.address));
  }
  std::set<std::string> referenced;
  for (const Action &a : rows) {
    // Python's index key: "<trace_key>:<action_id>" (pendings.py:257-263).
    const std::string member = view.trace_id + ":" + a.action_id;
    for (const std::string &account : a.accounts) {
      payload.aai.emplace_back(account, member);
      if (tx_accounts.count(account) == 0) {
        referenced.insert(account);
      }
    }
  }
  // The referenced_accounts channel is an unauthenticated remote-kill surface:
  // ton-trace-ttl-tracker splits the payload on ';' and indexes [1] with no
  // length check, and a panic there takes the whole process down (§1, §11).
  // Safe here by construction — an account is "wc:HEX", which cannot contain a
  // separator — but the string must stay BUILT, never interpolated from
  // anything less constrained.
  payload.referenced_accounts.assign(referenced.begin(), referenced.end());
  return payload;
}

}  // namespace

void EmuClassifierActor::start_up() {
  if (!cfg_.capture_dir.empty()) {
    auto status = td::mkpath(cfg_.capture_dir + "/");
    if (status.is_error()) {
      LOG(ERROR) << "[mch-emu] capture dir '" << cfg_.capture_dir
                 << "' unusable: " << status.move_as_error();
    }
  }
  last_stats_us_ = emu_now_us();
  alarm_timestamp() = td::Timestamp::in(kStatsIntervalSec);
}

void EmuClassifierActor::note_shed(EmuClassifyOutcome why) {
  switch (why) {
    case EmuClassifyOutcome::shed_admission: stats_.shed_admission++; break;
    case EmuClassifyOutcome::shed_oversize: stats_.shed_oversize++; break;
    case EmuClassifyOutcome::bypassed_disabled: stats_.bypassed_disabled++; break;
    default: break;  // the classifier-side outcomes never come through here
  }
}

bool EmuClassifierActor::remember_seen(const std::string &trace_id) {
  if (!seen_traces_.insert(trace_id).second) {
    return false;
  }
  seen_order_.push_back(trace_id);
  while (seen_order_.size() > kMaxSeenTraces) {
    seen_traces_.erase(seen_order_.front());
    seen_order_.pop_front();
  }
  return true;
}

std::unique_ptr<ClassifyFixpoint> EmuClassifierActor::make_fixpoint(PendingClassify &st) {
  // The builders hold references into `st`, which is heap-owned and never moved
  // for as long as the fixpoint can run another pass.
  return make_trace_fixpoint(st.res.trace_id, st.txs, st.view.interfaces, cfg_.prep->ir,
                             cfg_.prep->setup);
}

void EmuClassifierActor::classify(EmuTraceView view, td::Promise<EmuClassifyResult> promise) {
  auto st = std::make_unique<PendingClassify>();
  st->view = std::move(view);
  st->promise = std::move(promise);
  st->res.trace_id = st->view.trace_id;
  st->started_us = emu_now_us();
  st->deadline_us = st->view.deadline_us;
  // No anchor handed back (W3/§3): every anchor-backed cell is read and encoded
  // before the promise is answered, so the view's lifetime is the only one that
  // ever mattered.

  st->res.queue_us = st->started_us - st->view.sent_us;
  if (st->res.queue_us > 0 && static_cast<std::size_t>(st->res.queue_us) > stats_.queue_us_max) {
    stats_.queue_us_max = static_cast<std::size_t>(st->res.queue_us);
  }

  // Attribution first, so `emissions` stays the count of everything that
  // reached the actor — including what the deadline then sheds.
  stats_.emissions++;
  if (!remember_seen(st->view.trace_id)) {
    stats_.reemissions++;
  }
  if (!st->view.nodes.empty()) {
    stats_.by_finality[static_cast<std::size_t>(st->view.nodes.front().finality)]++;
  }
  if (st->view.tx_limit_exceeded) {
    stats_.tx_limit_exceeded++;
  }

  // The backlog valve (§16.2), checked before any work: a burst sheds instead
  // of piling on. It bounds the QUEUE WAIT only. With tier 2 wired the deadline
  // gains a SECOND role it did not have in EM1.5 — it is re-checked before each
  // fetch round (§5.4) — but never DURING work, so no partially classified or
  // partially serialized result can be discarded mid-flight.
  if (st->started_us > st->deadline_us) {
    stats_.shed_deadline++;
    st->res.outcome = EmuClassifyOutcome::shed_deadline;
    // §7 empty-degraded: no `actions` field at all (actions_blob stays empty),
    // only the marker. Degrades to TODAY's normal — a pending trace with no
    // actions yet — which every consumer already handles.
    st->res.payload.state = "shed_deadline";
    // Not a latency sample: no work was done, and counting a ~0 us shed would
    // dilute the average this feature is judged on.
    answer(std::move(st), /*record_latency=*/false);
    return;
  }

  // The TLB parsers throw vm::VmError, which does NOT derive from std::exception
  // (excno.hpp) and which tdactor does not catch anywhere — upstream contains it
  // with the try/catch around parse_transactions' body, and the off-pipeline seam
  // is outside that. Unguarded, one malformed cell would abort the emulator.
  try {
    // Before conversion on purpose: a trace that fails to convert is exactly the
    // one that cannot be reproduced offline any other way.
    if (!cfg_.capture_dir.empty()) {
      auto status = write_capture(cfg_.capture_dir, st->view, cfg_.global_version);
      if (status.is_error()) {
        LOG(WARNING) << "[mch-emu] trace=" << st->view.trace_id
                     << " capture failed: " << status.move_as_error();
      }
    }

    // ONCE per emission, however many passes run (§4.1): this is the expensive
    // half, and the fixpoint's per-round rebuild is the cheap O(#tx) half.
    auto r_txs = emu_to_schema_txs(st->view, cfg_.global_version);
    if (r_txs.is_error()) {
      stats_.convert_failed++;
      st->res.outcome = EmuClassifyOutcome::convert_failed;
      // §7: empty-degraded and FORCED, not chosen — the basic-actions fallback
      // needs parsed transactions, and conversion is what produces them.
      st->res.payload.state = "convert_failed";
      st->res.failure = true;
      // A category so the caller never sees failure with `none`; NOT counted in
      // by_category, which attributes the engine's own failures (convert_failed
      // is its own counter).
      st->res.failure_category = FailureCategory::malformed_trace;
      st->res.failure_reason = r_txs.move_as_error().message().str();
      LOG(WARNING) << "[mch-emu] trace=" << st->view.trace_id
                   << " CONVERT FAILED: " << st->res.failure_reason;
      answer(std::move(st), /*record_latency=*/true);
      return;
    }
    st->txs = r_txs.move_as_ok();
  } catch (const vm::VmError &e) {
    stats_.convert_failed++;
    st->res.outcome = EmuClassifyOutcome::convert_failed;
    st->res.payload.state = "convert_failed";
    st->res.failure = true;
    st->res.failure_category = FailureCategory::malformed_trace;
    st->res.failure_reason = std::string("exception (vm): ") + e.get_msg();
    LOG(WARNING) << "[mch-emu] trace=" << st->res.trace_id << " EXCEPTION (vm): " << e.get_msg();
    answer(std::move(st), /*record_latency=*/true);
    return;
  } catch (...) {
    stats_.convert_failed++;
    st->res.outcome = EmuClassifyOutcome::convert_failed;
    st->res.payload.state = "convert_failed";
    st->res.failure = true;
    st->res.failure_category = FailureCategory::malformed_trace;
    st->res.failure_reason = "unknown exception";
    LOG(WARNING) << "[mch-emu] trace=" << st->res.trace_id << " EXCEPTION";
    answer(std::move(st), /*record_latency=*/true);
    return;
  }

  st->fx = make_fixpoint(*st);
  start_or_queue(std::move(st));
}

void EmuClassifierActor::start_or_queue(std::unique_ptr<PendingClassify> st) {
  // §5.2: a trace is only ever "in flight" here while SUSPENDED — a pass that
  // needs no fetch runs to completion inside one mailbox turn — so this queues
  // exactly the emissions that would otherwise race an outstanding fetch and
  // let emission N finish after N+1. That inversion is harmless for stats and a
  // correctness bug for the write-back, whose own actions_finality guard is the
  // primary defence (it also covers the reordering this cannot see: two
  // TraceInserter actors contending for the same connection pool).
  if (inflight_traces_.count(st->res.trace_id) != 0) {
    queued_[st->res.trace_id].push_back(std::move(st));
    return;
  }
  run_pass(std::move(st));
}

void EmuClassifierActor::run_pass(std::unique_ptr<PendingClassify> st) {
  FixpointStep step;
  try {
    step = st->fx->step();
  } catch (const std::exception &e) {
    st->res.failure = true;
    st->res.failure_reason = std::string("exception: ") + e.what();
    st->res.failure_category = FailureCategory::engine_fault;
    finish(std::move(st), {}, {}, 0);
    return;
  } catch (...) {
    st->res.failure = true;
    st->res.failure_reason = "unknown exception";
    st->res.failure_category = FailureCategory::engine_fault;
    finish(std::move(st), {}, {}, 0);
    return;
  }
  if (step == FixpointStep::committed) {
    commit_pass(std::move(st));
    return;
  }

  // The pass wants keys. EVERY reason not to fetch commits the pass just run
  // (§4.1/§8.1): a trace that classifies fine today must never classify worse
  // because tier 2 is enabled, slow, or broken.
  const std::int64_t now_us = emu_now_us();
  if (!cfg_.tier2_fetch) {
    commit_pass(std::move(st));  // tier 2 off — round 0, exactly as in EM1.5
    return;
  }
  if (now_us > st->deadline_us) {
    tier2_.deadline_hits++;
    commit_pass(std::move(st));
    return;
  }
  if (breaker_open(now_us)) {
    tier2_.breaker_open++;
    commit_pass(std::move(st));  // zero latency: the socket is not touched
    return;
  }
  if (pending_.size() >= kMaxSuspended) {
    tier2_.shed_suspended++;
    commit_pass(std::move(st));
    return;
  }
  LookupWants wanted = st->fx->wanted();  // copied BEFORE feed() clears it
  suspend(std::move(st), std::move(wanted));
}

void EmuClassifierActor::suspend(std::unique_ptr<PendingClassify> st, LookupWants wanted) {
  const std::uint64_t id = next_pending_id_++;
  st->id = id;
  st->suspended_us = emu_now_us();
  inflight_traces_.insert(st->res.trace_id);
  tier2_.batches++;

  // The response carries the ID, never a pointer into `pending_`: a batch that
  // comes back after the sweep committed its trace then finds no entry and is a
  // clean no-op. ~LambdaPromise answers with an error if the fetch is dropped
  // (lookup-actor death, shutdown), so on_lookup_failed still resumes the trace
  // and the promise the emulator is waiting on cannot be lost.
  auto self = actor_id(this);
  auto P = td::PromiseCreator::lambda(
      [self, id](td::Result<std::map<std::string, Value>> R) mutable {
        if (R.is_error()) {
          td::actor::send_closure(self, &EmuClassifierActor::on_lookup_failed, id,
                                  R.move_as_error());
        } else {
          td::actor::send_closure(self, &EmuClassifierActor::on_lookup_batch, id,
                                  R.move_as_ok());
        }
      });

  pending_.emplace(id, std::move(st));
  tier2_.suspended_max = std::max(tier2_.suspended_max, pending_.size());
  arm_sweep();
  // Registered FIRST, so a fetch that answers inline still finds the state.
  cfg_.tier2_fetch(std::move(wanted), std::move(P));
}

std::unique_ptr<PendingClassify> EmuClassifierActor::take_pending(std::uint64_t id) {
  auto it = pending_.find(id);
  if (it == pending_.end()) {
    return nullptr;  // swept and already answered
  }
  auto st = std::move(it->second);
  pending_.erase(it);
  return st;
}

void EmuClassifierActor::on_lookup_batch(std::uint64_t id, std::map<std::string, Value> values) {
  note_fetch_ok();
  auto st = take_pending(id);
  if (st == nullptr) {
    return;
  }
  const std::int64_t elapsed = emu_now_us() - st->suspended_us;
  if (elapsed > 0) {
    auto us = static_cast<std::size_t>(elapsed);
    tier2_.fetch_us_total += us;
    tier2_.fetch_us_max = std::max(tier2_.fetch_us_max, us);
  }
  for (const auto &[key, v] : values) {
    if (v.is_null()) {
      tier2_.absent++;
    } else {
      tier2_.served++;
    }
  }
  st->fx->feed(values);
  run_pass(std::move(st));
}

void EmuClassifierActor::on_lookup_failed(std::uint64_t id, td::Status why) {
  // Counted even for a state the sweep already took: the STORE failed, and that
  // is what the breaker is watching, not this particular trace's fate.
  tier2_.fetch_errors++;
  note_fetch_failed(emu_now_us());
  auto st = take_pending(id);
  if (st == nullptr) {
    return;
  }
  const std::int64_t elapsed = emu_now_us() - st->suspended_us;
  if (elapsed > 0) {
    auto us = static_cast<std::size_t>(elapsed);
    tier2_.fetch_us_total += us;
    tier2_.fetch_us_max = std::max(tier2_.fetch_us_max, us);
  }
  LOG(DEBUG) << "[mch-emu] trace=" << st->res.trace_id << " tier-2 fetch failed: " << why;
  // §8.1: IDENTICAL to an all-null batch. feed({}) fills every outstanding key
  // with an explicit null, which is exactly what an all-null answer does — so
  // the trace takes the same next pass and produces the same bytes. That
  // identity is what the S7 gate asserts.
  st->fx->feed({});
  run_pass(std::move(st));
}

void EmuClassifierActor::commit_pass(std::unique_ptr<PendingClassify> st) {
  st->fx->commit_current();
  // Holds the arena the CoreActions point into: it must outlive finish(), which
  // is where build_action and the serializer read them. It dies at the end of
  // THIS function, after finish() has returned.
  DriverResult dr = st->fx->take_result();
  std::vector<Action> rows;
  std::vector<std::string> matcher_names;
  std::size_t scrubbed = 0;
  rows_from_pass(dr, st->res, rows, matcher_names, scrubbed, tier2_);
  finish(std::move(st), rows, matcher_names, scrubbed);
}

void EmuClassifierActor::finish(std::unique_ptr<PendingClassify> st,
                                const std::vector<Action> &rows,
                                const std::vector<std::string> &matcher_names,
                                std::size_t scrubbed) {
  EmuClassifyResult &res = st->res;
  // SERIALIZE HERE, inside the arena/anchor lifetime (§3). Timed on its own:
  // it lands in the same critical section as classify_us but is a different
  // cost with a different fix. The single serialization call site — the
  // suspension loop wraps the call, it does not move it.
  const std::int64_t ser_started_us = emu_now_us();
  ActionSerializeStats ser;
  // Python's trace.start_lt is the ROOT tx lt (event_deserializer.py:150),
  // which is min(lt) over a well-formed trace — the same value
  // fill_trace_aggregates derives, so the score matches Python's ZADD.
  // (Redis scores are IEEE-754 doubles, so an lt above 2^53 loses
  // precision — a pre-existing property of the protocol, identical for
  // both writers, §4.4.)
  std::int64_t aai_score = 0;
  bool first_tx = true;
  for (const auto &tx : st->txs) {
    const auto lt = static_cast<std::int64_t>(tx.lt);
    aai_score = first_tx ? lt : std::min(aai_score, lt);
    first_tx = false;
  }
  res.payload = build_payload(st->view, rows, aai_score, ser);
  res.serialize_us = emu_now_us() - ser_started_us;
  if (res.serialize_us > 0) {
    auto us = static_cast<std::size_t>(res.serialize_us);
    stats_.serialize_us_total += us;
    stats_.serialize_us_max = std::max(stats_.serialize_us_max, us);
  }
  stats_.actions_blob_bytes += res.payload.actions_blob.size();
  stats_.ser_float_values += ser.float_values;
  stats_.ser_cell_values += ser.cell_values;
  stats_.ser_unrenderable += ser.unrenderable;
  if (ser.float_values != 0 || ser.cell_values != 0 || ser.unrenderable != 0) {
    LOG(WARNING) << "[mch-emu] trace=" << res.trace_id
                 << " serializer saw float=" << ser.float_values
                 << " cell=" << ser.cell_values << " unrenderable=" << ser.unrenderable;
  }

  stats_.actions += rows.size();
  if (res.used_fallback) {
    stats_.fallback_actions += rows.size();
  }
  stats_.unported_btypes += res.unported_btypes;
  stats_.lookups.tier1_hits += res.lookup_stats.tier1_hits;
  stats_.lookups.tier2_hits += res.lookup_stats.tier2_hits;
  stats_.lookups.misses += res.lookup_stats.misses;
  for (const auto &[kind, n] : res.lookup_stats.misses_by_kind) {
    stats_.lookups.misses_by_kind[kind] += n;
  }
  if (scrubbed > 0) {
    stats_.arena_refs_scrubbed += scrubbed;
    LOG(WARNING) << "[mch-emu] trace=" << res.trace_id << " scrubbed " << scrubbed
                 << " arena block reference(s) out of the action rows";
  }
  if (res.failure) {
    stats_.failed++;
    stats_.by_category[res.failure_category]++;
    res.outcome = EmuClassifyOutcome::classify_failed;
    // §7: the blob IS written — it carries the basic-actions fallback rows,
    // which is the degraded output the Q4 policy requires, not silence.
    res.payload.state = "fallback";
    LOG(WARNING) << "[mch-emu] trace=" << res.trace_id << " FAILED ("
                 << category_name(res.failure_category) << "): " << res.failure_reason;
  } else {
    stats_.classified++;
    res.outcome = EmuClassifyOutcome::classified;
    // §7: `unknown` marks a clean classification that matched nothing. The
    // row ships either way, so the state string is the ONLY thing that
    // separates it from a trace with genuine protocol actions.
    // The test is exact, not a heuristic: create_unknown_action is the sole
    // producer of that type, and it only runs when the matched set was
    // empty — so a lone "unknown" row means precisely that branch fired.
    const bool unknown_row = rows.size() == 1 && rows.front().type == "unknown";
    res.payload.state = unknown_row ? "unknown" : "ok";
    std::string names;
    for (const std::string &n : matcher_names) {
      if (!names.empty()) names += ",";
      names += n;
    }
    // §5: size AND per-path latency, so the ungated (finalized) population
    // can be watched separately from the emulated one that carries the tail.
    LOG(DEBUG) << "[mch-emu] trace=" << res.trace_id << " txs=" << st->txs.size()
               << " actions=" << res.payload.action_count
               << " classify_us=" << (emu_now_us() - st->started_us)
               << " serialize_us=" << res.serialize_us << " finality="
               << (st->view.nodes.empty()
                       ? -1
                       : static_cast<int>(st->view.nodes.front().finality))
               << " matchers=[" << names << "]";
  }
  answer(std::move(st), /*record_latency=*/true);
}

void EmuClassifierActor::answer(std::unique_ptr<PendingClassify> st, bool record_latency) {
  if (record_latency) {
    // WALL time, so it still measures what the insert waited for; with tier 2
    // on it therefore INCLUDES the fetch round trips, which tier2{fetch_us}
    // reports separately — classify_us minus fetch_us is the CPU half.
    st->res.classify_us = emu_now_us() - st->started_us;
    if (st->res.classify_us > 0) {
      auto us = static_cast<std::size_t>(st->res.classify_us);
      stats_.classify_us_total += us;
      if (us > stats_.classify_us_max) {
        stats_.classify_us_max = us;
      }
    }
    stats_.latency_samples++;
  }

  const std::string trace_id = st->res.trace_id;
  auto promise = std::move(st->promise);
  EmuClassifyResult res = std::move(st->res);
  // The view (and with it the cell anchor and the converted transactions) is
  // released BEFORE the insert runs: the payload is plain strings, so nothing
  // downstream needs it.
  st.reset();
  // The single answer: after this the sink's continuation runs the insert.
  promise.set_value(std::move(res));
  // Only THEN the next emission of this trace, so their inserts stay ordered.
  release_trace(trace_id);
}

void EmuClassifierActor::release_trace(const std::string &trace_id) {
  inflight_traces_.erase(trace_id);
  auto it = queued_.find(trace_id);
  if (it == queued_.end()) {
    return;
  }
  auto st = std::move(it->second.front());
  it->second.pop_front();
  if (it->second.empty()) {
    queued_.erase(it);
  }
  // May finish synchronously and release again; the recursion is bounded by the
  // queue length, i.e. by how many emissions of ONE trace arrived during a
  // single fetch round (2-3 in practice, since a trace is emitted 2-3 times in
  // its whole life).
  run_pass(std::move(st));
}

bool EmuClassifierActor::breaker_open(std::int64_t now_us) {
  if (breaker_until_us_ == 0) {
    return false;
  }
  if (now_us < breaker_until_us_) {
    return true;
  }
  // Expired: let ONE trial request through. Success closes the breaker;
  // failure re-opens it immediately, because breaker_failures_ is still at the
  // threshold.
  breaker_until_us_ = 0;
  LOG(WARNING) << "[mch-emu] tier-2 breaker half-open, trying the store again";
  return false;
}

void EmuClassifierActor::note_fetch_ok() {
  if (breaker_failures_ != 0) {
    LOG(WARNING) << "[mch-emu] tier-2 store answering again (breaker closed)";
  }
  breaker_failures_ = 0;
  breaker_until_us_ = 0;
}

void EmuClassifierActor::note_fetch_failed(std::int64_t now_us) {
  breaker_failures_++;
  if (breaker_failures_ < kBreakerFailures) {
    return;
  }
  if (breaker_until_us_ == 0) {
    tier2_.breaker_trips++;
    LOG(WARNING) << "[mch-emu] tier-2 breaker OPEN for " << (kBreakerOpenUs / 1'000'000)
                 << "s after " << breaker_failures_
                 << " consecutive fetch failures; classification degrades to tier 1";
  }
  breaker_until_us_ = now_us + kBreakerOpenUs;
}

void EmuClassifierActor::sweep_pending(std::int64_t now_us) {
  // Collected first: committing a state answers a promise, which runs an
  // insert, which can start a queued emission that suspends and inserts into
  // pending_ — mutating the map under an iterator.
  std::vector<std::uint64_t> due;
  for (const auto &[id, st] : pending_) {
    if (now_us > st->deadline_us + kSweepGraceUs) {
      due.push_back(id);
    }
  }
  for (std::uint64_t id : due) {
    auto st = take_pending(id);
    if (st == nullptr) {
      continue;
    }
    tier2_.swept++;
    LOG(WARNING) << "[mch-emu] trace=" << st->res.trace_id
                 << " tier-2 response never arrived (" << (now_us - st->suspended_us) / 1000
                 << " ms), committing the current pass";
    // The retained pass, with every outstanding key left null. NOT another
    // classify pass: the point is to release the insert promise, and a trace
    // whose store is gone will not classify differently for having tried again.
    commit_pass(std::move(st));
  }
}

void EmuClassifierActor::arm_sweep() {
  // The stats line is 10 s away; a suspended trace needs to be looked at within
  // one sweep interval, so pull the alarm in whenever it sits further out.
  const td::Timestamp sweep = td::Timestamp::in(kSweepIntervalSec);
  if (get_alarm_timestamp().at() > sweep.at()) {
    alarm_timestamp() = sweep;
  }
}

void EmuClassifierActor::alarm() {
  const std::int64_t now_us = emu_now_us();
  // FIRST, and independent of the stats cadence (§5.5): a suspended trace whose
  // response is lost pins finalized_blocks_inflight_ until this runs.
  sweep_pending(now_us);

  const auto stats_period_us = static_cast<std::int64_t>(kStatsIntervalSec * 1e6);
  if (now_us - last_stats_us_ >= stats_period_us) {
    log_stats();
    last_stats_us_ = now_us;
  }
  // Re-arm on the shorter of the two, and only pay the 1 Hz wakeup while
  // something is actually suspended (with tier 2 off, nothing ever is).
  const double to_stats =
      std::max(0.0, kStatsIntervalSec - static_cast<double>(now_us - last_stats_us_) / 1e6);
  alarm_timestamp() =
      td::Timestamp::in(pending_.empty() ? to_stats : std::min(to_stats, kSweepIntervalSec));
}

void EmuClassifierActor::log_stats() {
  std::string cats;
  for (const auto &[cat, n] : stats_.by_category) {
    if (!cats.empty()) cats += ",";
    cats += std::string(category_name(cat)) + "=" + std::to_string(n);
  }
  // Per-kind miss attribution: with tier 2 unwired in EM1.5 and no dedust_pool
  // detector in the emulator, the total alone cannot separate an expected gap
  // from a real resolution failure.
  std::string miss_kinds;
  for (const auto &[kind, n] : stats_.lookups.misses_by_kind) {
    if (!miss_kinds.empty()) miss_kinds += ",";
    miss_kinds += kind + "=" + std::to_string(n);
  }
  // The tier-2 DEMAND profile — what a store would have to serve — as opposed to
  // misses_by_kind, which is what went unserved.
  std::string req_kinds;
  for (const auto &[kind, n] : tier2_.requested_by_kind) {
    if (!req_kinds.empty()) req_kinds += ",";
    req_kinds += kind + "=" + std::to_string(n);
  }
  std::size_t resolved = stats_.lookups.tier1_hits + stats_.lookups.tier2_hits;
  LOG(INFO) << "[mch-emu] emissions=" << stats_.emissions << " (+"
            << (stats_.emissions - prev_.emissions) << ")"
            << " reemissions=" << stats_.reemissions
            << " classified=" << stats_.classified << " (+"
            << (stats_.classified - prev_.classified) << ")"
            << " failed=" << stats_.failed << " convert_failed=" << stats_.convert_failed
            << " tx_limit_exceeded=" << stats_.tx_limit_exceeded
            << " actions=" << stats_.actions << " (+" << (stats_.actions - prev_.actions) << ")"
            << " fallback_actions=" << stats_.fallback_actions
            << " unported_btypes=" << stats_.unported_btypes
            << " arena_refs_scrubbed=" << stats_.arena_refs_scrubbed
            << " finality{emulated=" << stats_.by_finality[0]
            << " confirmed=" << stats_.by_finality[1]
            << " finalized=" << stats_.by_finality[2] << "}"
            << " [" << cats << "]"
            << " lookups{tier1=" << stats_.lookups.tier1_hits
            << " tier2=" << stats_.lookups.tier2_hits << " miss=" << stats_.lookups.misses
            << " miss_by_kind={" << miss_kinds << "}"
            << " tier2_rate=" << (resolved ? (100 * stats_.lookups.tier2_hits / resolved) : 0)
            << "%}"
            << " shed{admission=" << stats_.shed_admission
            << " deadline=" << stats_.shed_deadline
            // §5: should read zero in normal traffic. A non-zero count means the
            // cap is refusing real work, i.e. the cap is wrong, not the traffic.
            << " oversize=" << stats_.shed_oversize
            << " disabled=" << stats_.bypassed_disabled << "}"
            << " in_flight=" << cfg_.gate->in_flight.load(std::memory_order_relaxed)
            << (cfg_.gate->disabled.load(std::memory_order_relaxed) ? " DISABLED" : "")
            << " tier2{rounds=" << tier2_.rounds
            << " exhausted=" << tier2_.rounds_exhausted
            << " requested=" << tier2_.requested
            << " served=" << tier2_.served << " absent=" << tier2_.absent
            << " by_kind={" << req_kinds << "}"
            // The suspension group (§5.5-§5.7). Every counter here is a reason a
            // trace committed WITHOUT the keys it wanted, so a non-zero one
            // explains a tier-2 hit rate that looks worse than the store's.
            << " batches=" << tier2_.batches << " errors=" << tier2_.fetch_errors
            << " fetch_us{avg="
            << (tier2_.batches ? tier2_.fetch_us_total / tier2_.batches : 0)
            << " max=" << tier2_.fetch_us_max << "}"
            << " suspended=" << pending_.size() << "/" << tier2_.suspended_max
            << " deadline=" << tier2_.deadline_hits << " swept=" << tier2_.swept
            << " shed_suspended=" << tier2_.shed_suspended
            << " breaker{trips=" << tier2_.breaker_trips << " served_open="
            << tier2_.breaker_open << (breaker_until_us_ != 0 ? " OPEN" : "") << "}"
            << " wrong_value_guard=" << tier2_.wrong_value_guard << "}"
            << " queue_us{max=" << stats_.queue_us_max << "}"
            << " classify_us{avg="
            << (stats_.latency_samples ? stats_.classify_us_total / stats_.latency_samples : 0)
            << " max=" << stats_.classify_us_max << "}"
            // §3: reported SEPARATELY from classify_us. The two share a critical
            // section but not a cause, and the composed per-block budget is
            // traces x (rounds x classify_us + serialize_us) — only derivable
            // if the halves stay apart.
            << " serialize_us{avg="
            << (stats_.latency_samples ? stats_.serialize_us_total / stats_.latency_samples : 0)
            << " max=" << stats_.serialize_us_max << "}"
            << " actions_bytes=" << stats_.actions_blob_bytes
            // Should read all zeros forever; a non-zero is a fill producing a
            // Value shape §2.7 says never reaches an Action.
            << " ser{float=" << stats_.ser_float_values << " cell=" << stats_.ser_cell_values
            << " unrenderable=" << stats_.ser_unrenderable << "}";
  prev_ = stats_;
}

}  // namespace mch
