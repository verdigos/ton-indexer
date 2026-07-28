// EM1 bridge (em1-design §1/§3): the ONLY translation unit that knows the
// emulator's ::Trace. Kept in its own target (mch-emu-bridge) so that mch-emu
// stays linkable without emulator_static.
#pragma once

#include "EmuClassifierActor.h"
#include "EmuTypes.h"

#include "Measurement.h"  // MeasurementPtr

#include "td/actor/actor.h"
#include "td/utils/Status.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>

struct Trace;  // ::Trace — the emulator's; mch::Trace also exists

namespace mch {

using EmuTraceSink = std::function<void(::Trace, td::Promise<td::Unit>, MeasurementPtr)>;

// Serializes each tx cell (EmuTypes.h: the snapshot must outlive the block's
// cell store) and copies small PODs; no cell is parsed. May also THROW —
// callers must contain it, see decorate_sink.
td::Result<EmuTraceSnapshot> snapshot_trace(const ::Trace &trace);

// Wraps the emulator's insert sink so every emission also reaches the shadow
// classifier. The inner sink is called unchanged and unconditionally.
EmuTraceSink decorate_sink(EmuTraceSink inner, td::actor::ActorId<EmuClassifierActor> classifier,
                           std::shared_ptr<std::atomic<std::size_t>> pending);

}  // namespace mch
