// EM1.5 bridge (em1-design §13/§15): the ONLY translation unit that knows the
// emulator's ::Trace. Kept in its own target (mch-emu-bridge) so that mch-emu
// stays linkable without emulator_static.
#pragma once

#include "EmuClassifierActor.h"
#include "EmuTypes.h"

#include "Measurement.h"  // MeasurementPtr

#include "td/actor/actor.h"

#include <functional>
#include <memory>

struct Trace;  // ::Trace — the emulator's; mch::Trace also exists

namespace mch {

// What the four trace sources call. UNCHANGED by EM2 W5 on purpose (§4.2): the
// payload is bridged inside TraceScheduler, so no listener and no block emulator
// learns that the write-back exists.
using EmuTraceSink = std::function<void(::Trace, td::Promise<td::Unit>, MeasurementPtr)>;

// What the INSERT takes (§4.2). decorate_sink consumes one of these and returns
// an EmuTraceSink, which is the whole shape of the seam widening.
using EmuInsertSink =
    std::function<void(::Trace, EmuActionPayload, td::Promise<td::Unit>, MeasurementPtr)>;

// Copies td::Refs and small PODs plus the trace's cell anchor: NO cell is
// loaded, so this cannot fail and cannot throw on the emulator's own stack.
EmuTraceView make_view(const ::Trace &trace);

// Wraps the emulator's insert sink so every emission is classified first and
// the inner sink runs in the response continuation. The inner sink is called
// exactly once on EVERY path — shed, deadline, disabled, oversize, failure,
// success (§16.5) — carrying the classifier's payload where one was produced and
// an empty payload (with the §7 state marker) where it was not; in `nonblocking`
// mode it is called immediately instead and the response is used for stats only.
EmuTraceSink decorate_sink(EmuInsertSink inner, td::actor::ActorId<EmuClassifierActor> classifier,
                           std::shared_ptr<EmuGate> gate, bool nonblocking,
                           std::size_t max_nodes = kMaxClassifyNodes);

}  // namespace mch
