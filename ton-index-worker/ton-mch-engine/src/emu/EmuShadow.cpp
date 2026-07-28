#include "EmuShadow.h"

#include "EmuInterfaces.h"

#include "TraceEmulator.h"  // ::Trace / ::TraceNode / FinalityState

#include "td/utils/base64.h"
#include "td/utils/logging.h"
#include "td/utils/overloaded.h"
#include "vm/boc.h"

#include <exception>
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

}  // namespace

td::Result<EmuTraceSnapshot> snapshot_trace(const ::Trace &trace) {
  EmuTraceSnapshot snap;
  snap.trace_id = td::base64_encode(trace.ext_in_msg_hash_norm.as_slice());
  snap.tx_limit_exceeded = trace.tx_limit_exceeded;

  std::vector<const TraceNode *> stack;
  if (trace.root) {
    stack.push_back(trace.root.get());
  }
  while (!stack.empty()) {
    const TraceNode *node = stack.back();
    stack.pop_back();
    EmuTxRec rec;
    rec.address = node->address;
    // Same call as the Redis path (convert::to_bytes, Serializer.hpp), so the
    // bytes are the ones the emulator already produces for these cells.
    if (node->transaction_root.not_null()) {
      TRY_RESULT_ASSIGN(rec.tx_boc, vm::std_boc_serialize(node->transaction_root,
                                                          vm::BagOfCells::Mode::WithCRC32C));
    }
    rec.mc_seqno = node->mc_block_seqno;
    rec.finality = to_emu_finality(node->finality_state);
    snap.nodes.push_back(std::move(rec));
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
      snap.interfaces.emplace(addr, std::move(adapted));
    }
  }
  return snap;
}

EmuTraceSink decorate_sink(EmuTraceSink inner, td::actor::ActorId<EmuClassifierActor> classifier,
                           std::shared_ptr<std::atomic<std::size_t>> pending) {
  return [inner = std::move(inner), classifier, pending = std::move(pending)](
             ::Trace trace, td::Promise<td::Unit> promise, MeasurementPtr measurement) {
    // Snapshot BEFORE the inner sink consumes the trace.
    if (pending->fetch_add(1, std::memory_order_relaxed) >= kMaxPendingSnapshots) {
      pending->fetch_sub(1, std::memory_order_relaxed);
      td::actor::send_closure(classifier, &EmuClassifierActor::note_dropped);
    } else {
      // Serializing the tx cells walks them, so unlike the old Ref copy this CAN
      // fail and CAN throw (vm::VmError does not derive from std::exception, and
      // this runs inside the emulator's call stack): nothing may escape.
      td::Status status;
      try {
        auto r_snap = snapshot_trace(trace);
        if (r_snap.is_error()) {
          status = r_snap.move_as_error();
        } else {
          td::actor::send_closure(classifier, &EmuClassifierActor::classify, r_snap.move_as_ok());
        }
      } catch (const vm::VmError &e) {
        status = td::Status::Error(PSLICE() << "exception (vm): " << e.get_msg());
      } catch (const std::exception &e) {
        status = td::Status::Error(PSLICE() << "exception: " << e.what());
      } catch (...) {
        status = td::Status::Error("unknown exception");
      }
      if (status.is_error()) {
        // Same rollback as the drop path: the classify that would have
        // decremented `pending` never ran.
        pending->fetch_sub(1, std::memory_order_relaxed);
        td::actor::send_closure(classifier, &EmuClassifierActor::note_snapshot_failed);
        LOG(WARNING) << "[mch-emu] trace="
                     << td::base64_encode(trace.ext_in_msg_hash_norm.as_slice())
                     << " SNAPSHOT FAILED: " << status;
      }
    }
    inner(std::move(trace), std::move(promise), std::move(measurement));
  };
}

}  // namespace mch
