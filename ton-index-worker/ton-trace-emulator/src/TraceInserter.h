#pragma once
#include "Measurement.h"
#include <sw/redis++/redis++.h>
#include "td/actor/actor.h"
#include "crypto/common/bitstring.h"
#include "TraceEmulator.h"
// EM2 §4.1: plain strings + PODs only. This is the one mch header the insert TU
// includes, deliberately — no Value.h, no ActionBuild.h, no arena semantics in a
// file whose body runs blocking Redis round-trips.
#include "emu/EmuActionPayload.h"

// EM2 §8 phase 2: where the write-back's two extra keys land. Defaults are the
// PRODUCTION names, so a run with no flags writes exactly what Python writes
// today; the shadow run points them at non-colliding names that no consumer
// reads (ConvertHSet reads only root_node, the node keys and `actions`).
struct ActionWriteConfig {
    std::string actions_field{"actions"};
    std::string aai_prefix{"_aai:"};
    // §4.3a stores the emission's finality beside the blob and reads it back
    // before overwriting. Derived from actions_field so a phase-2 run guards its
    // own key rather than production's.
    std::string finality_field() const { return actions_field + "_finality"; }
};

class ITraceInsertManager : public td::actor::Actor {
public:
    // EM2 §4.2: the payload is a parameter of the INSERT, not a property of the
    // trace. Widening here (rather than hanging it off ::Trace, which already
    // took cell_anchor from us) keeps the 3-arg sink the four trace sources see
    // completely unchanged — TraceScheduler bridges the two shapes.
    virtual void insert(Trace trace, mch::EmuActionPayload actions, td::Promise<td::Unit> promise,
                        MeasurementPtr measurement) = 0;
};

class RedisInsertManager: public ITraceInsertManager {
    sw::redis::Redis redis_;
    ActionWriteConfig action_cfg_;

public:
    RedisInsertManager(std::string redis_dsn, ActionWriteConfig action_cfg = {}) :
        redis_(sw::redis::Redis(redis_dsn)), action_cfg_(std::move(action_cfg)) {}

    void insert(Trace trace, mch::EmuActionPayload actions, td::Promise<td::Unit> promise,
                MeasurementPtr measurement) override;
};

// --mch-classify-only: removes the per-trace Redis WRITE only. The input channel
// subscription, health hash and invalidated-trace tracking are scheduler-owned
// and keep running, so a first shadow run cannot disturb production pending data.
class NullTraceInsertManager: public ITraceInsertManager {
public:
    void insert(Trace trace, mch::EmuActionPayload actions, td::Promise<td::Unit> promise,
                MeasurementPtr measurement) override {
        promise.set_value(td::Unit());
    }
};

// --mch-actions-only (§8 phase 2): writes ONLY the write-back's own keys and
// NOTHING else — no node blobs, no interfaces, no account CAS, and critically no
// PUBLISH, because a second emulator republishing new_trace / new_finalized_txs /
// new_commited_txs into live consumers is the reason phase 2 is not simply "run
// another emulator".
//
// The HSET is wrapped in an EXISTS guard so this can never CREATE a trace hash:
// if our emulator wins the race against production's, the trace would otherwise
// exist with actions and no nodes, which ConvertHSet reads as a malformed trace.
// referenced_accounts is NOT published either — it would pollute the TTL
// tracker's per-trace address sets — so the _aai shadow keys are left to be
// dropped by hand after the run.
class ActionsOnlyInsertManager: public ITraceInsertManager {
    sw::redis::Redis redis_;
    ActionWriteConfig action_cfg_;

public:
    ActionsOnlyInsertManager(std::string redis_dsn, ActionWriteConfig action_cfg) :
        redis_(sw::redis::Redis(redis_dsn)), action_cfg_(std::move(action_cfg)) {}

    void insert(Trace trace, mch::EmuActionPayload actions, td::Promise<td::Unit> promise,
                MeasurementPtr measurement) override;
};
