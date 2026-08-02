#include "EmuShadow.h"

#include "EmuInterfaces.h"

#include "TraceEmulator.h"  // ::Trace / ::TraceNode / FinalityState

#include "td/utils/base64.h"
#include "td/utils/logging.h"
#include "td/utils/overloaded.h"

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

// Mark gate-side sheds so they remain distinct from a disabled classifier.
EmuActionPayload shed_payload(const char *state, EmuFinality finality) {
  EmuActionPayload p;
  p.state = state;
  p.finality = static_cast<std::uint8_t>(finality);
  return p;
}

// Returns a state string when the emulator-side gate rejects the emission.
const char *admit(EmuGate &gate, std::int64_t now_us,
                  td::actor::ActorId<EmuClassifierActor> classifier) {
  if (gate.disabled.load(std::memory_order_relaxed)) {
    td::actor::send_closure(classifier, &EmuClassifierActor::note_shed,
                            EmuClassifyOutcome::bypassed_disabled);
    return "disabled";
  }
  if (gate.in_flight.load(std::memory_order_relaxed) < kMaxInFlightClassify) {
    return nullptr;
  }
  // A full gate with no recent response triggers sticky fail-open. The initial
  // response timestamp lets this detect a stall on the first request.
  std::int64_t last = gate.last_response_us.load(std::memory_order_relaxed);
  if (now_us - last > kStallFailOpenUs &&
      !gate.disabled.exchange(true, std::memory_order_relaxed)) {
    LOG(ERROR) << "[mch-emu] classifier stalled (no response for " << (now_us - last) / 1000
               << " ms with " << kMaxInFlightClassify
               << " requests in flight), shadow classification DISABLED";
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
  // Keep the block's lazy cell store loadable for the classifier.
  view.anchor = trace.cell_anchor;
  // Tier-2 inputs. Both empty on the listener path; the hook then serves
  // nothing, producing a clean tier-2 miss.
  view.shard_states = trace.shard_states;
  view.config = trace.config;

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
      std::visit(
          td::overloaded(
              [&](const JettonWalletDetectorR::Result &r) { adapted.push_back(to_v2(r)); },
              [&](const NftItemDetectorR::Result &r) {
                auto v = to_v2(r);
                v.code_hash = code_hash;
                adapted.push_back(std::move(v));
              },
              [&](const GetGemsNftFixPriceSale::Result &r) {
                auto v = to_v2(r);
                v.code_hash = code_hash;
                adapted.push_back(std::move(v));
              },
              [&](const GetGemsNftAuction::Result &r) {
                auto v = to_v2(r);
                v.code_hash = code_hash;
                adapted.push_back(std::move(v));
              },
              // DeDust and nominator pools resolve in tier 1. nft_sale accepts
              // either fixed-price-sale variant.
              [&](const GetGemsNftFixPriceSaleV4::Result &r) {
                auto v = to_v2(r);
                v.code_hash = code_hash;
                adapted.push_back(std::move(v));
              },
              [&](const DedustPoolDetector::Result &r) { adapted.push_back(to_v2(r)); },
              [&](const MultisigOrder::Result &r) { adapted.push_back(to_v2(r)); },
              [&](const NominatorPoolContract::Result &r) { adapted.push_back(to_v2(r)); },
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
                           std::shared_ptr<EmuGate> gate) {
  return [inner = std::move(inner), classifier, gate = std::move(gate)](
             ::Trace trace, td::Promise<td::Unit> promise, MeasurementPtr measurement) {
    const std::int64_t now_us = emu_now_us();
    if (const char *shed = admit(*gate, now_us, classifier)) {
      // A shed has no view, so use the least-advanced finality.
      inner(std::move(trace), shed_payload(shed, EmuFinality::emulated), std::move(promise),
            std::move(measurement));
      return;
    }

    // This reference and POD walk does not load cells.
    EmuTraceView view = make_view(trace);
    view.sent_us = now_us;
    view.deadline_us = now_us + kClassifyDeadlineUs;
    const EmuFinality finality = view_finality(view);
    gate->in_flight.fetch_add(1, std::memory_order_relaxed);

    // The response continuation inserts exactly once and releases in_flight,
    // including when actor shutdown resolves the promise with an error.
    auto P = td::PromiseCreator::lambda([gate, inner, finality, trace = std::move(trace),
                                         promise = std::move(promise),
                                         measurement = std::move(measurement)](
                                            td::Result<EmuClassifyResult> R) mutable {
      gate->in_flight.fetch_sub(1, std::memory_order_relaxed);
      gate->last_response_us.store(emu_now_us(), std::memory_order_relaxed);
      // A lost classifier response still inserts blockchain data with a
      // response_lost marker and no action blob.
      EmuActionPayload payload;
      if (R.is_error()) {
        LOG(WARNING) << "[mch-emu] classify response lost: " << R.move_as_error();
        payload = shed_payload("response_lost", finality);
      } else {
        payload = std::move(R.move_as_ok().payload);
      }
      inner(std::move(trace), std::move(payload), std::move(promise), std::move(measurement));
    });
    td::actor::send_closure(classifier, &EmuClassifierActor::classify, std::move(view),
                            std::move(P));
  };
}

}  // namespace mch
