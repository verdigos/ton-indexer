#include "Tier2SuspendTest.h"

#include "BlockTree.h"
#include "ClassifyDriver.h"
#include "emu/EmuClassifierActor.h"

#include "td/actor/actor.h"
#include "td/utils/Time.h"

#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mch {

namespace {

int g_pass = 0;
int g_fail = 0;

void check(const std::string &name, bool ok, const std::string &detail = {}) {
  if (ok) {
    g_pass++;
    std::printf("PASS %s\n", name.c_str());
  } else {
    g_fail++;
    std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : " -- ", detail.c_str());
  }
}

void check_eq(const std::string &name, std::size_t got, std::size_t want) {
  check(name, got == want,
        got == want ? "" : "got " + std::to_string(got) + ", want " + std::to_string(want));
}

// --- the scripted fixpoint ---------------------------------------------------

// Per-emission script state, shared by the two builders one fixpoint holds.
// `passes` and `resolved` are the only two things this gate needs to observe,
// and both are encoded into the committed trace so they come back out as
// PAYLOAD BYTES rather than as counters the test could read from the inside.
struct Script {
  std::string trace_id;
  std::size_t passes = 0;
  bool resolved = false;
};

// The lookup key the scripted source demands. Shape matters only in that it is
// a single Str address, like every registered lookup.
const char *kScriptAddr = "0:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

// A one-transaction trace built in code. mch::Trace/Transaction/Message are
// plain structs, so the whole block substrate exists without a fixture file and
// without a transaction cell. `start_lt`/`end_lt` carry the script's state:
// with no matchers the classify pass matches nothing, ClassifyCore raises
// `unknown_trace`, and create_unknown_action copies those aggregates straight
// into the row the serializer packs. That is what makes the committed pass
// observable as bytes.
void fill_trace(Trace &trace, const std::string &trace_id, std::int64_t start_lt,
                std::int64_t end_lt) {
  trace.trace_id = trace_id;
  trace.start_lt = start_lt;
  trace.end_lt = end_lt;
  trace.start_utime = 111;
  trace.end_utime = 222;
  trace.mc_seqno_end = 333;

  auto tx = std::make_unique<Transaction>();
  tx->hash = "TXHASH0000000000000000000000";
  tx->lt = start_lt;
  tx->now = 111;
  tx->account = std::string("0:") + std::string(64, 'B');
  tx->descr = "ord";
  tx->orig_status = "active";
  tx->end_status = "active";

  auto msg = std::make_unique<Message>();
  msg->msg_hash = "MSGHASH000000000000000000000";
  msg->tx_hash = tx->hash;
  msg->tx_lt = tx->lt;
  msg->direction = "in";
  msg->destination = tx->account;
  msg->value = 1;
  msg->tx = tx.get();
  tx->messages.push_back(std::move(msg));

  trace.transactions.push_back(std::move(tx));
}

// Never consulted: the scripted matcher set is empty, so the engine runs no
// build program. It exists because classify_trace dereferences the source.
class NoLookupSource : public LookupSource {
 public:
  Value fetch(const std::string &, const std::vector<Value> &) const override {
    return Value::null();
  }
};

// An empty artifact: no matchers, so a pass produces no actions and the trace
// commits as `unknown`. The fixpoint holds these by reference, hence statics.
const LoadedIr &empty_ir() {
  static const LoadedIr ir;
  return ir;
}
const ClassifySetup &empty_setup() {
  static const ClassifySetup setup;
  return setup;
}

std::unique_ptr<ClassifyFixpoint> scripted_fixpoint(const std::string &trace_id) {
  auto s = std::make_shared<Script>();
  s->trace_id = trace_id;

  ContextBuilder make_ctx = [s]() -> td::Result<std::unique_ptr<TraceContext>> {
    s->passes++;
    auto ctx = std::make_unique<TraceContext>();
    fill_trace(ctx->trace, s->trace_id, 1000 + static_cast<std::int64_t>(s->passes),
               s->resolved ? 777 : 555);
    ctx->tree = to_tree(ctx->trace);
    if (ctx->tree.root == nullptr) {
      return td::Status::Error("scripted trace produced an empty event tree");
    }
    ctx->root = init_block(ctx->arena, ctx->tree.root);
    return ctx;
  };

  // The RoundSourceBuilder is documented as free to ignore the recording hook.
  // This one does the opposite: it CALLS the hook, which is exactly what the
  // engine does when a matcher evaluates `lookup` — minus the engine. The hook
  // returns the already-fetched value when there is one, so the script learns
  // whether the round trip resolved without reaching into the fixpoint.
  RoundSourceBuilder make_src = [s](TraceContext &ctx,
                                    ParsedBlockLookupSource::Tier2Hook record) {
    Value v = record("jetton_wallet", std::vector<Value>{Value::make_str(kScriptAddr)});
    s->resolved = !v.is_null();
    // Stamped HERE, not in make_ctx: a pass's context is built before the hook
    // can answer, so make_ctx only ever sees the PREVIOUS pass's verdict.
    // end_lt is a trace aggregate create_unknown_action copies into the row and
    // nothing in the tree derives from it, so writing it after the tree is
    // built is safe.
    ctx.trace.end_lt = s->resolved ? 777 : 555;
    return RoundSource{std::make_shared<NoLookupSource>(), nullptr};
  };

  return std::make_unique<ClassifyFixpoint>(std::move(make_ctx), std::move(make_src), empty_ir(),
                                            empty_setup(), kMaxTier2Rounds);
}

class ScriptedClassifier : public EmuClassifierActor {
 public:
  using EmuClassifierActor::EmuClassifierActor;

 protected:
  std::unique_ptr<ClassifyFixpoint> make_fixpoint(PendingClassify &st) override {
    return scripted_fixpoint(st.res.trace_id);
  }
};

// --- the fake store ----------------------------------------------------------

// Every batch the classifier issues lands here and stays until the TEST decides
// what happens to it: answered with values, answered with nulls, failed, or
// left outstanding forever. Holding the promise is what makes "never answers"
// real — dropping it would answer with an error via ~LambdaPromise and resume
// the trace, which is the opposite of the case under test.
struct Fake {
  std::mutex m;
  std::vector<std::pair<LookupWants, td::Promise<std::map<std::string, Value>>>> pending;
  std::atomic<std::size_t> calls{0};
};

enum class Answer { nulls, hit, error };

EmuTier2Fetch fake_fetch(Fake *f) {
  return [f](LookupWants wanted, td::Promise<std::map<std::string, Value>> promise) {
    f->calls++;
    std::lock_guard<std::mutex> g(f->m);
    f->pending.emplace_back(std::move(wanted), std::move(promise));
  };
}

// Answering a promise runs its lambda HERE, and that lambda does a
// send_closure — which needs a scheduler context on the calling thread. The
// caller therefore runs this inside scheduler->run_in_context; without it the
// resume message is simply never delivered and every suspended trace waits for
// the sweep.
std::size_t drain_locked(Fake &f, Answer how) {
  std::vector<std::pair<LookupWants, td::Promise<std::map<std::string, Value>>>> batch;
  {
    std::lock_guard<std::mutex> g(f.m);
    batch.swap(f.pending);
  }
  for (auto &[wanted, promise] : batch) {
    if (how == Answer::error) {
      promise.set_error(td::Status::Error("fake store is down"));
      continue;
    }
    std::map<std::string, Value> values;
    for (const auto &[key, ka] : wanted) {
      values.emplace(key, how == Answer::hit ? Value::make_str("resolved") : Value::null());
    }
    promise.set_value(std::move(values));
  }
  return batch.size();
}

// --- the sink (what `inner` would be) ----------------------------------------

struct Sink {
  std::mutex m;
  std::vector<std::pair<std::string, std::string>> answers;  // (trace_id, blob), completion order
  std::atomic<std::size_t> count{0};
};

struct Harness {
  Fake fake;
  Sink sink;
  std::shared_ptr<EmuGate> gate = std::make_shared<EmuGate>();
  std::unique_ptr<td::actor::Scheduler> scheduler;
  td::actor::ActorOwn<ScriptedClassifier> actor;
  std::size_t sent = 0;

  // Mirrors decorate_sink exactly: bump in_flight, stamp sent/deadline, and run
  // the "insert" in the response continuation. If the actor ever answered twice
  // or not at all, this is where it shows.
  void emit(const std::string &trace_id, std::int64_t deadline_us) {
    sent++;
    EmuTraceView view;
    view.trace_id = trace_id;
    view.sent_us = emu_now_us();
    view.deadline_us = view.sent_us + deadline_us;
    gate->in_flight.fetch_add(1, std::memory_order_relaxed);
    Sink *sk = &sink;
    EmuGate *g = gate.get();
    auto P = td::PromiseCreator::lambda([sk, g, trace_id](td::Result<EmuClassifyResult> R) {
      g->in_flight.fetch_sub(1, std::memory_order_relaxed);
      g->last_response_us.store(emu_now_us(), std::memory_order_relaxed);
      std::string blob = R.is_ok() ? R.ok().payload.actions_blob : std::string("<ERROR>");
      {
        std::lock_guard<std::mutex> lock(sk->m);
        sk->answers.emplace_back(trace_id, std::move(blob));
      }
      sk->count++;
    });
    auto id = actor.get();
    scheduler->run_in_context([&] {
      td::actor::send_closure(id, &EmuClassifierActor::classify, std::move(view), std::move(P));
    });
  }

  std::size_t drain(Answer how) {
    std::size_t n = 0;
    scheduler->run_in_context([&] { n = drain_locked(fake, how); });
    return n;
  }

  // Pumps the scheduler until every emission so far has been answered.
  bool settle(double max_seconds) {
    const double until = td::Time::now() + max_seconds;
    while (td::Time::now() < until) {
      scheduler->run(0.01);
      if (sink.count.load() >= sent) {
        return true;
      }
    }
    return sink.count.load() >= sent;
  }

  void pump(double seconds) {
    const double until = td::Time::now() + seconds;
    while (td::Time::now() < until) {
      scheduler->run(0.01);
    }
  }

  std::string last_blob() {
    std::lock_guard<std::mutex> lock(sink.m);
    return sink.answers.empty() ? std::string() : sink.answers.back().second;
  }
  std::size_t answers_for(const std::string &trace_id) {
    std::lock_guard<std::mutex> lock(sink.m);
    std::size_t n = 0;
    for (const auto &[id, blob] : sink.answers) {
      if (id == trace_id) n++;
    }
    return n;
  }
};

constexpr std::int64_t kLongDeadlineUs = 60'000'000;

}  // namespace

int run_tier2_suspend_test() {
  Harness h;
  h.scheduler = std::make_unique<td::actor::Scheduler>(
      std::vector<td::actor::Scheduler::NodeInfo>{td::actor::Scheduler::NodeInfo{1}});

  EmuShadowConfig cfg;
  cfg.prep = std::make_shared<MchEnginePrep>();  // unused: the fixpoint is scripted
  cfg.gate = h.gate;
  cfg.tier2_fetch = {};  // phase 1 runs with tier 2 OFF
  h.scheduler->run_in_context([&] {
    h.actor = td::actor::create_actor<ScriptedClassifier>("ScriptedClassifier", cfg);
  });

  // Phases 1-5, 8 and 9 all compare PAYLOAD BYTES, and the payload carries the
  // trace id (create_unknown_action's action_id, and the wire's
  // trace_external_hash_norm) — so every emission in the comparison group has to
  // be the SAME trace, or the blobs differ for a reason that has nothing to do
  // with what is being tested. They run strictly one after another, so the
  // per-trace queue never holds more than one.
  const std::string kCmp = "TCMP";

  // --- 1. feature off: the collect pass records keys nobody fetches ----------
  h.emit(kCmp, kLongDeadlineUs);
  check("feature-off emission completes", h.settle(5.0));
  check_eq("feature-off issues no fetch", h.fake.calls.load(), 0);
  const std::string blob_round0 = h.last_blob();
  check("feature-off produced a payload", !blob_round0.empty());

  // Everything below runs with the fake wired in.
  {
    EmuShadowConfig on = cfg;
    on.tier2_fetch = fake_fetch(&h.fake);
    auto id = h.actor.get();
    // The config is per-actor state; rebuild the actor rather than mutate it
    // from another thread.
    h.scheduler->run_in_context([&] {
      h.actor = td::actor::create_actor<ScriptedClassifier>("ScriptedClassifier2", on);
    });
    (void)id;
  }

  // --- 2. all-null answer: a SECOND pass runs, with the nulls in hand --------
  h.emit(kCmp, kLongDeadlineUs);
  h.pump(0.05);
  check_eq("all-null: one batch issued", h.fake.calls.load(), 1);
  check_eq("all-null: the batch is answerable", h.drain(Answer::nulls), 1);
  check("all-null emission completes", h.settle(5.0));
  const std::string blob_null = h.last_blob();
  check("all-null commits a LATER pass than the tier-2-less run",
        !blob_null.empty() && blob_null != blob_round0);

  // --- 3. a real answer reaches the fixpoint --------------------------------
  h.emit(kCmp, kLongDeadlineUs);
  h.pump(0.05);
  h.drain(Answer::hit);
  check("hit emission completes", h.settle(5.0));
  const std::string blob_hit = h.last_blob();
  check("a resolved lookup changes the committed payload",
        !blob_hit.empty() && blob_hit != blob_null);

  // --- 4. THE byte-identity assertion (§8.1) --------------------------------
  const std::size_t calls_before_error = h.fake.calls.load();
  h.emit(kCmp, kLongDeadlineUs);
  h.pump(0.05);
  check_eq("error: one batch issued", h.fake.calls.load() - calls_before_error, 1);
  h.drain(Answer::error);
  check("errored emission completes", h.settle(5.0));
  check("an all-ERROR fetch is byte-identical to an all-NULL fetch",
        h.last_blob() == blob_null,
        "error blob " + std::to_string(h.last_blob().size()) + " B vs null blob " +
            std::to_string(blob_null.size()) + " B");

  // --- 5. a slow answer still lands -----------------------------------------
  const std::size_t answered_before_delay = h.sink.count.load();
  h.emit(kCmp, kLongDeadlineUs);
  h.pump(0.20);  // the actor is suspended for the whole of this
  check_eq("delayed: nothing answered while the fetch is outstanding",
           h.sink.count.load() - answered_before_delay, 0);
  h.drain(Answer::nulls);
  check("delayed emission completes", h.settle(5.0));
  check("a delayed answer produces the same payload as a prompt one",
        h.last_blob() == blob_null);

  // --- 6. same-trace emissions complete IN ORDER (§5.2) ---------------------
  const std::size_t before_order = h.sink.count.load();
  const std::size_t calls_before_order = h.fake.calls.load();
  h.emit("TORDER", kLongDeadlineUs);
  h.emit("TORDER", kLongDeadlineUs);
  h.emit("TORDER", kLongDeadlineUs);
  h.pump(0.05);
  check_eq("ordering: only the FIRST emission of a trace is in flight",
           h.fake.calls.load() - calls_before_order, 1);
  for (int i = 0; i < 4 && h.sink.count.load() < before_order + 3; i++) {
    h.drain(Answer::nulls);
    h.pump(0.05);
  }
  check("all three same-trace emissions complete", h.settle(5.0));
  check_eq("three emissions, three answers", h.answers_for("TORDER"), 3);
  {
    // Same trace id, so completion order is the only thing to check, and the
    // queue is what guarantees it: emissions 2 and 3 never start until 1 has
    // answered, so their inserts cannot overtake it.
    std::lock_guard<std::mutex> lock(h.sink.m);
    bool contiguous = h.sink.answers.size() >= before_order + 3;
    for (std::size_t i = before_order; contiguous && i < before_order + 3; i++) {
      contiguous = h.sink.answers[i].first == "TORDER";
    }
    check("the three answers are contiguous and in send order", contiguous);
  }

  // --- 7. suspended cap (§5.7): over it, commit instead of suspending -------
  const std::size_t calls_before_cap = h.fake.calls.load();
  const std::size_t answered_before_cap = h.sink.count.load();
  for (std::size_t i = 0; i < kMaxSuspended + 4; i++) {
    h.emit("TCAP" + std::to_string(i), kLongDeadlineUs);
  }
  h.pump(0.50);
  check_eq("the suspended cap bounds outstanding fetches",
           h.fake.calls.load() - calls_before_cap, kMaxSuspended);
  check_eq("emissions over the cap commit immediately instead of suspending",
           h.sink.count.load() - answered_before_cap, 4);
  for (int i = 0; i < 6 && h.sink.count.load() < h.sent; i++) {
    h.drain(Answer::nulls);
    h.pump(0.05);
  }
  check("every capped-phase emission completes", h.settle(10.0));

  // --- 8. never answered: the sweep is what releases the insert (§5.5) ------
  const std::size_t answered_before_sweep = h.sink.count.load();
  h.emit(kCmp, 50'000);  // 50 ms deadline; the fetch is never answered
  h.pump(0.10);
  check_eq("swept: nothing answered while the response is missing",
           h.sink.count.load(), answered_before_sweep);
  // deadline + kSweepGraceUs + up to one sweep interval.
  check("a trace whose response never arrives is still answered", h.settle(4.0));
  check("the sweep commits the pass that was already run",
        h.last_blob() == blob_round0);

  // --- 9. circuit breaker (§5.6): 5 failures, then no socket at all ---------
  // The swept trace's fetch is still outstanding. Answer it SUCCESSFULLY first:
  // a late answer for a state the sweep already committed is a no-op for the
  // trace but still tells the breaker the store is alive, so the failure run
  // below starts from a closed breaker rather than from a stray failure.
  h.drain(Answer::nulls);
  h.pump(0.05);
  const std::size_t calls_before_breaker = h.fake.calls.load();
  for (std::size_t i = 0; i < kBreakerFailures; i++) {
    h.emit("TBRK" + std::to_string(i), kLongDeadlineUs);
    h.pump(0.05);
    h.drain(Answer::error);
    h.settle(5.0);
  }
  check_eq("breaker: each of the first five emissions fetched once",
           h.fake.calls.load() - calls_before_breaker, kBreakerFailures);
  const std::size_t calls_at_trip = h.fake.calls.load();
  for (int i = 0; i < 4; i++) {
    h.emit("TOPEN" + std::to_string(i), kLongDeadlineUs);
  }
  h.emit(kCmp, kLongDeadlineUs);
  check("post-trip emissions complete", h.settle(5.0));
  check_eq("an OPEN breaker serves every trace without touching the store",
           h.fake.calls.load() - calls_at_trip, 0);
  check("a breaker-open trace commits the pass it had, exactly like tier 2 off",
        h.last_blob() == blob_round0);

  // --- invariants over the whole run ---------------------------------------
  h.drain(Answer::error);  // release anything still held
  h.pump(0.10);
  check_eq("exactly one answer per emission", h.sink.count.load(), h.sent);
  check_eq("in_flight returns to zero", h.gate->in_flight.load(), 0);
  check("the fail-open watchdog never tripped", !h.gate->disabled.load());

  h.scheduler->run_in_context([&] { h.actor = {}; });
  h.scheduler->run(0.05);
  h.scheduler->stop();
  h.scheduler.reset();

  std::printf("\n%d/%d tier-2 suspension checks passed\n", g_pass, g_pass + g_fail);
  return g_fail == 0 ? 0 : 1;
}

}  // namespace mch
