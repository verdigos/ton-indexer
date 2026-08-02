// Bridge for the emulator's ::Trace type. Its separate target keeps mch-emu
// linkable without emulator_static.
#pragma once

#include "EmuClassifierActor.h"
#include "EmuTypes.h"

#include "Measurement.h"  // MeasurementPtr

#include "td/actor/actor.h"

#include <functional>
#include <memory>

struct Trace;  // Emulator ::Trace; mch::Trace also exists.

namespace mch {

// Trace-source sink; TraceScheduler supplies the writeback payload.
using EmuTraceSink = std::function<void(::Trace, td::Promise<td::Unit>, MeasurementPtr)>;

// decorate_sink adapts this payload-aware sink to EmuTraceSink.
using EmuInsertSink =
    std::function<void(::Trace, EmuActionPayload, td::Promise<td::Unit>, MeasurementPtr)>;

// Copies references, PODs, and the cell anchor without loading cells.
EmuTraceView make_view(const ::Trace &trace);

// Classifies each emission before invoking the inner sink exactly once. Failure
// paths carry an empty payload with an outcome marker.
EmuTraceSink decorate_sink(EmuInsertSink inner, td::actor::ActorId<EmuClassifierActor> classifier,
                           std::shared_ptr<EmuGate> gate);

}  // namespace mch
