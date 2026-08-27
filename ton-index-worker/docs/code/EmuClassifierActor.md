---
module: EmuClassifierActor
source: ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp
source: ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.h
source-commit: 06d4c50
---

# EmuClassifierActor

Actor that turns one emulated trace view into classified action rows and the serialized Redis writeback payload, answering each request exactly once through its promise; it performs no Redis or database writes itself. The classify entry converts the view to schema transactions, runs schema classification with an optional trace-scoped tier-2 celldb lookup, serializes the resulting rows while the view's cell anchor is still alive, and records per-emission telemetry into cumulative stats. Runs as a td actor, so all state is single-threaded per worker; workers are sharded by index and each owns its own reemission LRU, so a round-robin pool undercounts reemissions. Key invariant: every path, including VM faults that do not derive from std::exception, reaches the single promise answer.

## class mch::EmuClassifierStats

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.h:20`

Cumulative per-worker counters for outcomes, latency, lookups, and serializer anomalies.

Plain aggregate; alarm prints totals plus the delta against the actor's prev_ snapshot, which is a copy of this struct taken at the previous log line. ser_cell_values and ser_unrenderable aggregate ActionSerializeStats across emissions; by_finality buckets the root node's finality and by_category buckets failure outcomes.

## class mch::EmuClassifierActor

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.h:36`

Actor wrapper classifying trace views and building the serialized action payload with telemetry.

Owns the classifier config, worker index, cumulative stats plus the previous-line snapshot, and the bounded seen-traces set with its eviction queue. classify is the scheduler's request entry and always answers exactly once; alarm emits the periodic stats line and start_up schedules the first one. No storage I/O happens here — the sink's continuation runs the insert after the promise resolves.

## build_payload

`EmuActionPayload build_payload(const EmuTraceView & view, const std::vector<Action> & rows, std::int64_t aai_score, ActionSerializeStats & st)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:100`
- callers: mch::EmuClassifierActor::classify (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:203)
- calls: `mch::EmuActionPayload::EmuActionPayload`, `mch::serialize_actions`

Assembles the Redis writeback payload from the serialized action rows.

Serializes the rows through serialize_actions while the view is alive, then derives per-action routes of type plus accounts and account-to-member pairs of the form trace id, colon, action id, preserving action ids byte-for-byte. Sets action_count and carries the caller's aai_score; classify saves and restores finality and update_seq around the call because the freshly built payload does not carry them.

## start_up

`void start_up()`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:123`
- callers: none found in parsed TU set

Schedules the first periodic stats alarm ten seconds out.


## remember_seen

`bool remember_seen(const std::string & trace_id)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:127`
- callers: mch::EmuClassifierActor::classify (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:154)

Records a trace id in the bounded LRU; false means reemission.

Insertion into the set decides first sight; a duplicate returns false without touching the queue, so an id keeps its original eviction position. Once the queue exceeds 65536 entries the oldest id is erased from both structures. Dedup is per-worker, so round-robin pools undercount reemissions.

## classify

`void classify(EmuTraceView view, std::int64_t enqueued_us, td::Promise<EmuClassifyResult> promise)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:139`
- callers: none found in parsed TU set
- calls: `mch::ActionSerializeStats::ActionSerializeStats`, `mch::EmuClassifierActor::remember_seen`, `mch::EmuClassifyResult::EmuClassifyResult`, `mch::build_payload`, `mch::category_name`, `mch::classify_txs`, `mch::emu_now_us`, `mch::emu_to_schema_txs`, `mch::record_phase_exception`, `mch::view_finality`

Classifies one trace view and answers the promise exactly once with result and payload.

Opens with telemetry: queue delay from the enqueue timestamp, emission and reemission counts, root-node finality bucket, and the tx-limit flag. The try body converts the view to schema transactions; a conversion failure marks convert_failed with category malformed_trace and skips classification. Otherwise classify_txs runs, the AAI score becomes the minimum transaction LT — chosen because Redis sorted-set scores lose integer precision above 2^53 — and build_payload serializes the rows inside the view's cell-anchor lifetime with serialize time measured separately. Post-conversion failures keep the basic-action fallback payload with state fallback; success sets state ok, or unknown when a synthetic unknown row was emitted. VM fault types are caught explicitly because they do not derive from std::exception, and every catch funnels through record_phase_exception before the single promise answer.

```mermaid
flowchart TD
  A["telemetry: queue delay, emission, finality bucket"] --> B["emu_to_schema_txs"]
  B -- "error" --> C["outcome convert_failed, state convert_failed"]
  B -- "ok" --> D["classify_txs with tier-2 lookups"]
  D --> F["build_payload: serialize rows, aai_score = min tx LT"]
  F --> G{"res.failure"}
  G -- "yes" --> H["state fallback, count failed"]
  G -- "no" --> I{"unknown_row"}
  I -- "yes" --> J["state unknown"]
  I -- "no" --> K["state ok, count classified"]
  B -. "exception" --> X["record_phase_exception"]
  D -. "exception" --> X
  F -. "exception" --> X
  X --> Y{"past_conversion"}
  Y -- "no" --> Z["convert_failed, malformed_trace"]
  Y -- "yes" --> W["classify_failed, engine_fault, state fallback"]
  C --> P["promise set exactly once"]
  H --> P
  J --> P
  K --> P
  Z --> P
  W --> P
```

## category_name

`const char * category_name(FailureCategory c)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:23`
- callers: mch::EmuClassifierActor::classify (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:247); mch::EmuClassifierActor::alarm (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:304)

Maps a failure category to its stable log-line name; an out-of-range value yields a question mark.


## alarm

`void alarm()`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:300`
- callers: none found in parsed TU set
- calls: `mch::category_name`

Logs the cumulative stats line with deltas and reschedules itself.

Formats category counts, per-kind lookup misses, the tier-2 hit rate, and queue, classify, and serialize latency averages and maxima into one INFO line prefixed with the worker index. Per-kind miss attribution exists because a total alone cannot separate a kind with no lookup source, an expected gap, from a real resolution failure. Snapshots stats into prev_ and re-arms the alarm ten seconds out.


## record_phase_exception

`void record_phase_exception(EmuClassifierStats & stats, EmuClassifyResult & res, const std::string & message, bool past_conversion)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:33`
- callers: mch::EmuClassifierActor::classify (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:265); mch::EmuClassifierActor::classify (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:269); mch::EmuClassifierActor::classify (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:273); mch::EmuClassifierActor::classify (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:277); mch::EmuClassifierActor::classify (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:281)

Records an exception that escaped a classify phase into result and stats.

Marks the result failed with the message, then splits on past_conversion: before conversion counts convert_failed, sets category malformed_trace and state convert_failed; after conversion counts failed under engine_fault with state fallback, because rows already exist and the fallback payload is still written. by_category is bumped only on the post-conversion arm. Called from all five catch handlers in classify.

## classify_txs

`void classify_txs(const EmuClassifierConfig & cfg, const EmuTraceView & view, const std::vector<schema::Transaction> & txs, EmuClassifyResult & res, std::vector<Action> & rows, std::vector<std::string> & matcher_names, std::size_t & scrubbed, bool & unknown_row)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:51`
- callers: mch::EmuClassifierActor::classify (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:188)
- calls: `mch::EmuCelldbTier2::EmuCelldbTier2`, `mch::EmuCelldbTier2::fetch`, `mch::EmuCelldbTier2::stats`, `mch::ParsedBlockLookupSource::ParsedBlockLookupSource`, `mch::ParsedBlockLookupSource::stats`, `mch::classify_schema_trace`

Runs schema classification over the converted transactions with trace-scoped tier-2 lookups.

Builds an EmuCelldbTier2 over the view's shard states for the lifetime of this classification and installs it as the lookup hook only when the config enables tier 2, then calls classify_schema_trace and copies failure fields, fallback usage, and lookup and tier-2 stats into the result. The interface map comes from the view, so emulated accounts resolve in tier 1. Any exception is caught here: VM fault types and std::exception map to engine_fault with a prefixed reason, and unknown exceptions are reported as such; nothing propagates to classify's own catch block from this helper.


## operator=

`EmuClassifierStats & operator=(const EmuClassifierStats &)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.h:20`
- callers: mch::EmuClassifierActor::alarm (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:346)

Implicit copy assignment; alarm uses it to snapshot stats into prev_ for delta reporting.


## EmuClassifierActor

`void EmuClassifierActor(EmuClassifierConfig cfg, std::size_t worker_index)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.h:38`
- callers: none found in parsed TU set
- calls: `mch::EmuClassifierConfig::EmuClassifierConfig`

Stores the config and worker index; every other member default-constructs.

