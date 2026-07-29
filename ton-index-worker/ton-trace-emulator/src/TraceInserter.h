#pragma once
#include "Measurement.h"
#include <sw/redis++/redis++.h>
#include "td/actor/actor.h"
#include "crypto/common/bitstring.h"
#include "TraceEmulator.h"
// Plain strings + PODs only. This is the one mch header the insert TU includes,
// deliberately — no Value.h, no ActionBuild.h, no arena semantics in a file
// whose body runs blocking Redis round-trips.
#include "emu/EmuActionPayload.h"


class ITraceInsertManager : public td::actor::Actor {
public:
    // The payload is a parameter of the INSERT, not a property of the trace.
    // Widening here (rather than hanging it off ::Trace, which already took
    // cell_anchor from us) keeps the 3-arg sink the four trace sources see
    // completely unchanged — TraceScheduler bridges the two shapes.
    virtual void insert(Trace trace, mch::EmuActionPayload actions, td::Promise<td::Unit> promise,
                        MeasurementPtr measurement) = 0;
};

class RedisInsertManager: public ITraceInsertManager {
    sw::redis::Redis redis_;

public:
    RedisInsertManager(std::string redis_dsn) :
        redis_(sw::redis::Redis(redis_dsn)) {}

    void insert(Trace trace, mch::EmuActionPayload actions, td::Promise<td::Unit> promise,
                MeasurementPtr measurement) override;
};
