#include "EmuClassifierActor.h"

#include "BlockTree.h"
#include "EmuActionSerialize.h"
#include "EmuTraceConvert.h"
#include "SchemaTraceLoader.h"

#include "vm/excno.hpp"  // vm::VmError — NOT a std::exception, needs its own arm

#include "td/utils/logging.h"

#include <algorithm>
#include <exception>
#include <set>
#include <string>
#include <utility>

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

// Value is not unconditionally self-contained (Value.h): VType::Block holds a
// RAW arena Block* that dies with the TraceContext this call owns. ActionBuild's
// fills go through av_addr/av_amount/dg, so a Block-typed value reaching an
// Action is believed impossible — this turns "believed impossible" into a
// counter instead of a use-after-free at the integration point.
// VType::Cell is deliberately left alone: those stay valid, which is what the
// anchor handed back in EmuClassifyResult is for.
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

// SAME SET AND ORDER as optional_fields() (EmuActionSerialize.cpp) — two lists
// that must stay in sync; a field added to Action needs a row in both.
std::size_t scrub_arena_refs(Action &a) {
  std::size_t n = 0;
  for (Value *v : {&a.source, &a.source_secondary, &a.destination, &a.destination_secondary,
                   &a.asset, &a.asset_secondary, &a.asset2, &a.asset2_secondary, &a.amount,
                   &a.value, &a.opcode, &a.ton_transfer_data, &a.jetton_transfer_data,
                   &a.jetton_swap_data, &a.nft_transfer_data, &a.nft_listing_data,
                   &a.nft_mint_data,
                   &a.dex_deposit_liquidity_data, &a.dex_withdraw_liquidity_data, &a.staking_data,
                   &a.evaa_withdraw_data, &a.vesting_send_message_data, &a.layerzero_packet_data,
                   &a.jvault_stake_data, &a.jvault_claim_data, &a.change_dns_record_data,
                   &a.extra}) {
    n += scrub_value(*v);
  }
  return n;
}

// Same sequence the deleted Phase-G shadow actor ran (schema_to_trace ->
// to_tree/init_block -> classify_trace over the two-tier lookup source), with
// the same try/catch and FailureCategory mapping, plus build_action run INSIDE
// the try — while the arena the CoreActions point into is still alive. Never
// throws.
void classify_txs(const EmuShadowConfig &cfg, const EmuTraceView &view,
                  const std::vector<schema::Transaction> &txs, EmuClassifyResult &res,
                  std::vector<Action> &rows, std::vector<std::string> &matcher_names,
                  std::size_t &scrubbed, bool &unknown_row) {
  const MchEnginePrep &prep = *cfg.prep;
  const ParsedBlockLookupSource::InterfaceMap &ifaces = view.interfaces;
  try {
    auto r_trace = schema_to_trace(res.trace_id, txs);
    if (r_trace.is_error()) {
      res.failure = true;
      res.failure_reason = r_trace.move_as_error().message().str();
      res.failure_category = FailureCategory::malformed_trace;
      return;
    }
    TraceContext ctx;
    ctx.trace = r_trace.move_as_ok();
    ctx.tree = to_tree(ctx.trace);
    if (ctx.tree.root == nullptr) {
      res.failure = true;
      res.failure_reason = "empty event tree";
      res.failure_category = FailureCategory::malformed_trace;
      return;
    }
    ctx.root = init_block(ctx.arena, ctx.tree.root);

    // One tier-2 instance per classify — it IS the scope the memo and both
    // budgets are measured over. Stack-lived, so it dies with this call and
    // nothing carries a cell reference past the view's lifetime.
    EmuCelldbTier2 tier2(&view.shard_states, view.config, cfg.tier2_budget);
    ParsedBlockLookupSource::Tier2Hook hook;
    if (cfg.tier2) {
      hook = [&tier2](const std::string &kind, const std::vector<Value> &args) {
        return tier2.fetch(kind, args);
      };
    }
    ParsedBlockLookupSource src(&ifaces, std::move(hook));
    ClassifyResult cres = classify_trace(ctx, *prep.matchers, prep.setup, src);
    res.failure = cres.failure;
    res.failure_reason = cres.failure_reason;
    res.failure_category = cres.failure_category;
    res.lookup_stats = src.stats();
    res.tier2_stats = tier2.stats();

    // On failure the regular actions are discarded and the basic-actions
    // fallback rows are what production commits (Classify.cpp).
    res.used_fallback = cres.failure;
    const std::vector<ActionRow> &core = cres.failure ? cres.fallback_rows : cres.action_rows;
    for (const ActionRow &r : core) {
      Action act;
      if (!build_action(r, act)) {  // btype outside the ported fill set
        res.unported_btypes++;
        continue;
      }
      scrubbed += scrub_arena_refs(act);
      rows.push_back(std::move(act));
    }
    // Debug-log telemetry: which matchers FIRED, off the fire list — a fire whose
    // product a later matcher absorbed contributes no row but did run.
    std::set<std::string> names;
    for (const CoreAction &a : cres.actions) {
      names.insert(a.matcher_name);
    }
    matcher_names.assign(names.begin(), names.end());
    // The synthetic row for a trace that classified cleanly and matched
    // nothing, so the trace ships "we looked and found nothing" instead of an
    // empty array that ton-index-go reports as a successful classification.
    // ctx.trace carries the aggregates schema_to_trace derived — the emulator
    // has no trace-level lt/utime of its own, and neither does Python here
    // (event_deserializer.py:146-151 takes max over the transactions too), so
    // the two agree by construction.
    if (cres.unknown_trace) {
      rows.push_back(create_unknown_action(ctx.trace));
      unknown_row = true;
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

// Everything the MULTI needs, built here while the view is still alive. The
// single serialization call site.
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
    // Python's index key: "<trace_key>:<action_id>" (pendings.py:257-263). The
    // suffix is action_id BYTE FOR BYTE, which is what lets a consumer join a
    // decoded action against its index member.
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
  // length check, and a panic there takes the whole process down. Safe here by
  // construction — an account is "wc:HEX", which cannot contain a separator —
  // but the string must stay BUILT, never interpolated from anything less
  // constrained.
  payload.referenced_accounts.assign(referenced.begin(), referenced.end());
  return payload;
}

}  // namespace

void EmuClassifierActor::start_up() {
  alarm_timestamp() = td::Timestamp::in(kStatsIntervalSec);
}

void EmuClassifierActor::note_shed(EmuClassifyOutcome why) {
  switch (why) {
    case EmuClassifyOutcome::shed_admission: stats_.shed_admission++; break;
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

void EmuClassifierActor::classify(EmuTraceView view, td::Promise<EmuClassifyResult> promise) {
  EmuClassifyResult res;
  res.trace_id = view.trace_id;
  // The payload's finality and the wire `finality` key are the SAME derivation
  // (view_finality), so the value the insert guards on cannot drift from the
  // one the blob advertises.
  res.payload.finality = static_cast<std::uint8_t>(view_finality(view));

  const std::int64_t started_us = emu_now_us();
  res.queue_us = started_us - view.sent_us;
  if (res.queue_us > 0 && static_cast<std::size_t>(res.queue_us) > stats_.queue_us_max) {
    stats_.queue_us_max = static_cast<std::size_t>(res.queue_us);
  }

  // Attribution first, so `emissions` stays the count of everything that
  // reached the actor — including what the deadline then sheds.
  stats_.emissions++;
  if (!remember_seen(view.trace_id)) {
    stats_.reemissions++;
  }
  if (!view.nodes.empty()) {
    stats_.by_finality[static_cast<std::size_t>(view.nodes.front().finality)]++;
  }
  if (view.tx_limit_exceeded) {
    stats_.tx_limit_exceeded++;
  }

  // The backlog valve (§16.2), checked before any work: a burst sheds instead
  // of piling on. It bounds the QUEUE WAIT only — tested once, here, and never
  // again while the trace is classified — so the latency this feature adds is
  // kClassifyDeadlineUs plus however long classify_txs takes.
  if (started_us > view.deadline_us) {
    stats_.shed_deadline++;
    res.outcome = EmuClassifyOutcome::shed_deadline;
    // The marker says WHY there is no payload; the blob stays empty, so the
    // `actions` field is not written and Go reports Classified:false.
    res.payload.state = "shed_deadline";
    promise.set_value(std::move(res));
    return;
  }

  // The rows this emission produced. They live HERE, not on the result: the
  // serializer runs below while `view` (and so the cell anchor a VType::Cell
  // value can point into) is still alive, and only bytes leave this function.
  std::vector<Action> rows;
  bool unknown_row = false;

  // The TLB parsers throw vm::VmError, which does NOT derive from std::exception
  // (excno.hpp) and which tdactor does not catch anywhere — upstream contains it
  // with the try/catch around parse_transactions' body, and the off-pipeline seam
  // is outside that. Unguarded, one malformed cell would abort the emulator.
  // Every arm falls through to the single promise.set_value below, which is what
  // lets the decorator's response continuation decrement EmuGate::in_flight on
  // the throwing path too.
  try {
    auto r_txs = emu_to_schema_txs(view, cfg_.global_version);
    if (r_txs.is_error()) {
      stats_.convert_failed++;
      res.outcome = EmuClassifyOutcome::convert_failed;
      res.failure = true;
      // A category so the caller never sees failure with `none`; NOT counted in
      // by_category, which attributes the engine's own failures (convert_failed
      // is its own counter).
      res.failure_category = FailureCategory::malformed_trace;
      res.failure_reason = r_txs.move_as_error().message().str();
      res.payload.state = "convert_failed";
      LOG(WARNING) << "[mch-emu] trace=" << view.trace_id
                   << " CONVERT FAILED: " << res.failure_reason;
    } else {
      auto txs = r_txs.move_as_ok();
      std::vector<std::string> matcher_names;
      std::size_t scrubbed = 0;
      classify_txs(cfg_, view, txs, res, rows, matcher_names, scrubbed, unknown_row);
      // Python's trace.start_lt is the ROOT tx lt (event_deserializer.py:150),
      // which is min(lt) over a well-formed trace — the same value
      // fill_trace_aggregates derives, so the score matches Python's ZADD.
      // (Redis scores are IEEE-754 doubles, so an lt above 2^53 loses
      // precision — a pre-existing property of the protocol, identical for both
      // writers.)
      std::int64_t aai_score = 0;
      bool first_tx = true;
      for (const auto &tx : txs) {
        const auto lt = static_cast<std::int64_t>(tx.lt);
        aai_score = first_tx ? lt : std::min(aai_score, lt);
        first_tx = false;
      }
      // SERIALIZE HERE, inside the view's (and therefore the anchor's)
      // lifetime. Timed on its own: it lands in the same critical section as
      // classify_us but is a different cost with a different fix.
      const std::int64_t ser_started_us = emu_now_us();
      ActionSerializeStats ser;
      const std::uint8_t finality = res.payload.finality;
      res.payload = build_payload(view, rows, aai_score, ser);
      res.payload.finality = finality;
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
      stats_.tier2.fetched += res.tier2_stats.fetched;
      stats_.tier2.memo_hits += res.tier2_stats.memo_hits;
      stats_.tier2.budget_exhausted += res.tier2_stats.budget_exhausted;
      if (scrubbed > 0) {
        stats_.arena_refs_scrubbed += scrubbed;
        LOG(WARNING) << "[mch-emu] trace=" << view.trace_id << " scrubbed " << scrubbed
                     << " arena block reference(s) out of the action rows";
      }
      if (res.failure) {
        stats_.failed++;
        stats_.by_category[res.failure_category]++;
        res.outcome = EmuClassifyOutcome::classify_failed;
        // The blob IS written — it carries the basic-actions fallback rows,
        // which is the degraded output the policy requires, not silence.
        res.payload.state = "fallback";
        LOG(WARNING) << "[mch-emu] trace=" << view.trace_id << " FAILED ("
                     << category_name(res.failure_category) << "): " << res.failure_reason;
      } else {
        stats_.classified++;
        res.outcome = EmuClassifyOutcome::classified;
        // `unknown` marks a clean classification that matched nothing. The row
        // ships either way, so the state string is the ONLY thing that
        // separates it from a trace with genuine protocol actions.
        res.payload.state = unknown_row ? "unknown" : "ok";
        std::string names;
        for (const std::string &n : matcher_names) {
          if (!names.empty()) names += ",";
          names += n;
        }
        LOG(DEBUG) << "[mch-emu] trace=" << view.trace_id << " txs=" << txs.size()
                   << " actions=" << res.payload.action_count
                   << " serialize_us=" << res.serialize_us << " matchers=[" << names << "]";
      }
    }
  } catch (const vm::VmError &e) {
    stats_.convert_failed++;
    res.outcome = EmuClassifyOutcome::convert_failed;
    res.failure = true;
    res.failure_category = FailureCategory::malformed_trace;
    res.failure_reason = std::string("exception (vm): ") + e.get_msg();
    res.payload.state = "convert_failed";
    LOG(WARNING) << "[mch-emu] trace=" << view.trace_id << " EXCEPTION (vm): " << e.get_msg();
  } catch (...) {
    stats_.convert_failed++;
    res.outcome = EmuClassifyOutcome::convert_failed;
    res.failure = true;
    res.failure_category = FailureCategory::malformed_trace;
    res.failure_reason = "unknown exception";
    res.payload.state = "convert_failed";
    LOG(WARNING) << "[mch-emu] trace=" << view.trace_id << " EXCEPTION";
  }

  res.classify_us = emu_now_us() - started_us;
  if (res.classify_us > 0) {
    auto us = static_cast<std::size_t>(res.classify_us);
    stats_.classify_us_total += us;
    if (us > stats_.classify_us_max) {
      stats_.classify_us_max = us;
    }
  }
  stats_.latency_samples++;

  // The single answer: after this the sink's continuation runs the insert.
  promise.set_value(std::move(res));
}

void EmuClassifierActor::alarm() {
  std::string cats;
  for (const auto &[cat, n] : stats_.by_category) {
    if (!cats.empty()) cats += ",";
    cats += std::string(category_name(cat)) + "=" + std::to_string(n);
  }
  // Per-kind miss attribution: the total alone cannot separate a kind with no
  // source (an expected gap) from a real resolution failure.
  std::string miss_kinds;
  for (const auto &[kind, n] : stats_.lookups.misses_by_kind) {
    if (!miss_kinds.empty()) miss_kinds += ",";
    miss_kinds += kind + "=" + std::to_string(n);
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
            << " tier2{fetched=" << stats_.tier2.fetched
            << " memo_hits=" << stats_.tier2.memo_hits
            << " budget_exhausted=" << stats_.tier2.budget_exhausted << "}"
            << " shed{admission=" << stats_.shed_admission
            << " deadline=" << stats_.shed_deadline
            << " disabled=" << stats_.bypassed_disabled << "}"
            << " in_flight=" << cfg_.gate->in_flight.load(std::memory_order_relaxed)
            << (cfg_.gate->disabled.load(std::memory_order_relaxed) ? " DISABLED" : "")
            << " queue_us{max=" << stats_.queue_us_max << "}"
            << " classify_us{avg="
            << (stats_.latency_samples ? stats_.classify_us_total / stats_.latency_samples : 0)
            << " max=" << stats_.classify_us_max << "}"
            << " serialize_us{avg="
            << (stats_.latency_samples ? stats_.serialize_us_total / stats_.latency_samples : 0)
            << " max=" << stats_.serialize_us_max << "}"
            << " blob_bytes=" << stats_.actions_blob_bytes
            << " ser{float=" << stats_.ser_float_values << " cell=" << stats_.ser_cell_values
            << " unrenderable=" << stats_.ser_unrenderable << "}";
  prev_ = stats_;
  alarm_timestamp() = td::Timestamp::in(kStatsIntervalSec);
}

}  // namespace mch
