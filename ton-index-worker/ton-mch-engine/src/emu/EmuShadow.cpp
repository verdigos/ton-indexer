#include "EmuShadow.h"

#include "EmuInterfaces.h"

#include "TraceEmulator.h"  // ::Trace / ::TraceNode / FinalityState

#include "td/utils/base64.h"
#include "td/utils/logging.h"
#include "td/utils/overloaded.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace mch {

namespace {

EmuFinality to_emu_finality(FinalityState f) {
  switch (f) {
    case FinalityState::Emulated: return EmuFinality::emulated;
    case FinalityState::Confirmed: return EmuFinality::confirmed;
    case FinalityState::Finalized: return EmuFinality::finalized;
  }
  return EmuFinality::emulated;
}

// §7's marker for a gate-side outcome. The classifier never saw the emission, so
// nothing downstream would set `state`, and leaving it null would make a shed
// indistinguishable from the feature being off.
EmuActionPayload shed_payload(const char *state, EmuFinality finality) {
  EmuActionPayload p;
  p.state = state;
  p.finality = static_cast<std::uint8_t>(finality);
  return p;
}

// The gate decision (§16.1/§16.3), taken on the emulator's own thread. Returns
// the §7 state string when the emission is turned away, or nullptr to admit it.
const char *admit(EmuGate &gate, std::int64_t now_us,
                  td::actor::ActorId<EmuClassifierActor> classifier) {
  if (gate.disabled.load(std::memory_order_relaxed)) {
    td::actor::send_closure(classifier, &EmuClassifierActor::note_shed,
                            EmuClassifyOutcome::bypassed_disabled);
    return "disabled";
  }
  // Staleness is tested REGARDLESS of in_flight (em2-tier2-design §5.6 item 2).
  // It used to live inside the "cap full" branch below, which cannot see a dead
  // tier-2 store: the block emulators self-throttle (kMaxFinalizedBlocksInFlight
  // = 2, kMaxConfirmedBlocksInFlight = 8, ~28 traces/block ≈ 280 block-sourced
  // traces in flight), so nothing guarantees the population ever reaches
  // kMaxInFlightClassify — a classifier wedged on a store that stopped answering
  // would stall the pipelines indefinitely while the watchdog never fired.
  // Sticky, and loud. No zero check — the gate seeds last_response_us at
  // construction, so staleness is measured from process start and a wedge on the
  // FIRST request trips this too.
  const std::int64_t last = gate.last_response_us.load(std::memory_order_relaxed);
  const bool stalled = now_us - last > kStallFailOpenUs;
  if (stalled) {
    if (!gate.disabled.exchange(true, std::memory_order_relaxed)) {
      LOG(ERROR) << "[mch-emu] classifier stalled (no response for " << (now_us - last) / 1000
                 << " ms, " << gate.in_flight.load(std::memory_order_relaxed)
                 << " requests in flight), shadow classification DISABLED";
    }
    // Attributed as bypassed_disabled, not shed_admission: the cap is not what
    // turned this emission away.
    td::actor::send_closure(classifier, &EmuClassifierActor::note_shed,
                            EmuClassifyOutcome::bypassed_disabled);
    return "disabled";
  }
  if (gate.in_flight.load(std::memory_order_relaxed) < kMaxInFlightClassify) {
    return nullptr;
  }
  td::actor::send_closure(classifier, &EmuClassifierActor::note_shed,
                          EmuClassifyOutcome::shed_admission);
  return "shed_admission";
}

}  // namespace

EmuTraceView make_view(const ::Trace &trace) {
  EmuTraceView view;
  view.trace_id = td::base64_encode(trace.ext_in_msg_hash_norm.as_slice());
  view.tx_limit_exceeded = trace.tx_limit_exceeded;
  // Everything the classifier will read off a block's lazy cell store stays
  // loadable through this handle, whatever the block emulator does next.
  view.anchor = trace.cell_anchor;

  std::vector<const TraceNode *> stack;
  if (trace.root) {
    stack.push_back(trace.root.get());
  }
  while (!stack.empty()) {
    const TraceNode *node = stack.back();
    stack.pop_back();
    EmuTxRef rec;
    rec.address = node->address;
    rec.tx_root = node->transaction_root;
    rec.mc_seqno = node->mc_block_seqno;
    rec.finality = to_emu_finality(node->finality_state);
    view.nodes.push_back(std::move(rec));
    for (auto it = node->children.rbegin(); it != node->children.rend(); ++it) {
      if (*it) {
        stack.push_back(it->get());
      }
    }
  }

  // trace.interfaces (not committed_interfaces): the trace-final map that goes
  // to Redis, so emulated accounts resolve too. Only the variants with a tier-1
  // kind are carried over; jetton master / nft collection have none.
  for (const auto &[addr, ifaces] : trace.interfaces) {
    std::vector<schema::BlockchainInterfaceV2> adapted;
    for (const auto &iface : ifaces) {
      std::visit(td::overloaded(
                     [&](const JettonWalletDetectorR::Result &r) { adapted.push_back(to_v2(r)); },
                     [&](const NftItemDetectorR::Result &r) { adapted.push_back(to_v2(r)); },
                     [&](const GetGemsNftFixPriceSale::Result &r) { adapted.push_back(to_v2(r)); },
                     [&](const GetGemsNftAuction::Result &r) { adapted.push_back(to_v2(r)); },
                     [](const JettonMasterDetectorR::Result &) {},
                     [](const NftCollectionDetectorR::Result &) {}),
                 iface);
    }
    if (!adapted.empty()) {
      view.interfaces.emplace(addr, std::move(adapted));
    }
  }
  return view;
}

EmuTraceSink decorate_sink(EmuInsertSink inner, td::actor::ActorId<EmuClassifierActor> classifier,
                           std::shared_ptr<EmuGate> gate, bool nonblocking,
                           std::size_t max_nodes) {
  // First N over-cap traces log their size, so the cap can be calibrated from
  // real traffic instead of guessed a second time.
  auto oversize_logged = std::make_shared<std::atomic<std::size_t>>(0);
  return [inner = std::move(inner), classifier, gate = std::move(gate), nonblocking, max_nodes,
          oversize_logged](::Trace trace, td::Promise<td::Unit> promise,
                           MeasurementPtr measurement) {
    const std::int64_t now_us = emu_now_us();
    if (const char *shed = admit(*gate, now_us, classifier)) {
      // The finality is unknown without a view, and building one for a shed
      // would defeat the valve — `emulated` keeps the guard conservative.
      inner(std::move(trace), shed_payload(shed, EmuFinality::emulated), std::move(promise),
            std::move(measurement));
      return;
    }

    // A Ref/POD walk only: nothing here can throw into the emulator's stack.
    EmuTraceView view = make_view(trace);
    view.sent_us = now_us;
    view.deadline_us = now_us + gate->classify_deadline_us;

    // §5 WORK CAP. This seam, and not the actor's deadline test, for a reason
    // beyond convenience: make_view already walked the tree so the count is
    // free, and refusing here means an oversized trace never consumes a queue
    // slot and never crosses a thread. Checking next to the deadline
    // (EmuClassifierActor.cpp) would also be before any parse work — but only
    // after queueing, which is the part that head-of-line blocks everything
    // behind it.
    if (view.nodes.size() > max_nodes) {
      td::actor::send_closure(classifier, &EmuClassifierActor::note_shed,
                              EmuClassifyOutcome::shed_oversize);
      if (oversize_logged->fetch_add(1, std::memory_order_relaxed) < 16) {
        LOG(WARNING) << "[mch-emu] trace=" << view.trace_id << " OVER WORK CAP ("
                     << view.nodes.size() << " nodes > " << max_nodes
                     << "), not classified; if this fires in normal traffic the cap is wrong";
      }
      // §5/Q3: no `actions` field at all, NOT a basic-actions fallback. The
      // fallback still needs the full parse pass over every node — the part that
      // scales with N — while producing a list of only ton_transfer /
      // call_contract / contract_deploy rows, which Go reports as a SUCCESSFUL
      // classification that happens to be missing every protocol action. "Not
      // classified yet" is a state consumers already handle; "classified,
      // wrongly" is not.
      inner(std::move(trace), shed_payload("shed_oversize", view_finality(view)),
            std::move(promise), std::move(measurement));
      return;
    }

    gate->in_flight.fetch_add(1, std::memory_order_relaxed);

    if (nonblocking) {
      // --mch-classify-nonblocking: EM1's shape — the insert does not wait, and
      // the response feeds the counters only. The payload is therefore NOT
      // available to the insert on this path; it stays a perf-measurement mode,
      // never a write-back one.
      auto P = td::PromiseCreator::lambda([gate](td::Result<EmuClassifyResult>) {
        gate->in_flight.fetch_sub(1, std::memory_order_relaxed);
        gate->last_response_us.store(emu_now_us(), std::memory_order_relaxed);
      });
      const EmuFinality finality = view_finality(view);
      td::actor::send_closure(classifier, &EmuClassifierActor::classify, std::move(view),
                              std::move(P));
      inner(std::move(trace), shed_payload("nonblocking", finality), std::move(promise),
            std::move(measurement));
      return;
    }

    // The insert runs in the response continuation, on the classifier's thread.
    // ~LambdaPromise answers a promise the actor never got to (shutdown) with an
    // error, so this fires exactly once even then — the insert cannot be lost.
    const EmuFinality finality = view_finality(view);
    auto P = td::PromiseCreator::lambda([gate, inner, finality, trace = std::move(trace),
                                         promise = std::move(promise),
                                         measurement = std::move(measurement)](
                                            td::Result<EmuClassifyResult> R) mutable {
      gate->in_flight.fetch_sub(1, std::memory_order_relaxed);
      gate->last_response_us.store(emu_now_us(), std::memory_order_relaxed);
      // W5: the payload rides into the MULTI from here. On a lost response there
      // is no payload to carry, so the insert still runs — suppressing it would
      // turn a classifier hiccup into a blockchain-data outage (§7) — with the
      // empty-degraded marker instead.
      EmuActionPayload payload;
      if (R.is_error()) {
        LOG(WARNING) << "[mch-emu] classify response lost: " << R.move_as_error();
        payload = shed_payload("response_lost", finality);
      } else {
        payload = std::move(R.move_as_ok().payload);
        payload.finality = static_cast<std::uint8_t>(finality);
      }
      inner(std::move(trace), std::move(payload), std::move(promise), std::move(measurement));
    });
    td::actor::send_closure(classifier, &EmuClassifierActor::classify, std::move(view),
                            std::move(P));
  };
}

}  // namespace mch
