#pragma once
#include "Measurement.h"
#include <sw/redis++/redis++.h>
#include "td/actor/actor.h"
#include "crypto/common/bitstring.h"
#include "TraceEmulator.h"
// Plain strings and PODs keep matcher value and arena types out of Redis writes.
#include "emu/EmuActionPayload.h"


class ITraceInsertManager : public td::actor::Actor {
public:
    // TraceScheduler bridges the three-argument trace sink to this payload-aware sink.
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
