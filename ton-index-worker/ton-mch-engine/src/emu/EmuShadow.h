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

using EmuTraceSink = std::function<void(::Trace, td::Promise<td::Unit>, MeasurementPtr)>;

// Copies td::Refs and small PODs plus the trace's cell anchor: NO cell is
// loaded, so this cannot fail and cannot throw on the emulator's own stack.
EmuTraceView make_view(const ::Trace &trace);

// Wraps the emulator's insert sink so every emission is classified first and
// the inner sink runs in the response continuation. The inner sink is called
// exactly once on EVERY path — shed, deadline, disabled, failure, success
// (§16.5); in `nonblocking` mode it is called immediately instead and the
// response is used for stats only.
EmuTraceSink decorate_sink(EmuTraceSink inner, td::actor::ActorId<EmuClassifierActor> classifier,
                           std::shared_ptr<EmuGate> gate, bool nonblocking);

}  // namespace mch
