# EM1 — in-process shadow classifier in ton-trace-emulator (design)

> **SUPERSEDED IN PART by §11-§20 (EM1.5) at the bottom of this file** (design, 2026-07-28). EM1 is
> committed as `6c6f3f4` and is the as-built record for §§1-2, §4, §6-§9. §3 (BOC snapshot +
> fire-and-forget sink) and §5's actor entry point are REPLACED by EM1.5: no serialization copy, a
> cell-lifetime anchor, and a request/response classify. Read §§11-20 for the current design.
>
> Status: DESIGN, awaiting lead ack. Scope = program plan `emu-integration-plan.md` §3 "EM1".
> Log-only: Redis writes byte-identical with and without the flag; absent flag = feature fully off.
> Verification for this phase is COMPILE ONLY (user directive) — see §9.
> Companion: `mch-cpp-prod-design.md` (Phase G `MchClassifierActor` precedent, reused where it fits).

## 0. Shape

```
listeners/emulators ──insert_trace_(Trace, Promise, Measurement)──> RedisInsertManager   [UNCHANGED]
                              │
                     decorated in the ctor (TraceScheduler.h:109-111)
                              └─ snapshot_trace(trace)  [tx cells → BOC bytes, no cell parsed]
                                    └─ send_closure ──> mch::EmuClassifierActor  [sibling, fire-and-forget]
                                          tx BOCs → schema::Transaction → schema_to_trace
                                          → to_tree/init_block → classify_trace(ParsedBlockLookupSource)
                                          → stats + LOG (+ optional capture dump)
```

Everything after `send_closure` runs on the classifier actor's own turn: no DB reads, no Redis, no
promise on the sink path, nothing inline in the block-completion chain (plan §3 v1.3 constraints).

## 1. Files

**New — target `mch-emu`** (`ton-mch-engine/src/emu/`, links `mch-classify` only, NO emulator dep):
- `EmuTypes.h` — `EmuFinality`, `EmuTxRec`, `EmuTraceSnapshot`, `EmuShadowConfig`, `kEmuGlobalVersion`.
- `EmuTraceConvert.{h,cpp}` — snapshot → `vector<schema::Transaction>` (§2).
- `EmuInterfaces.{h,cpp}` — detector `Result` → `schema::BlockchainInterfaceV2` (§4).
- `EmuClassifierActor.{h,cpp}` — the actor, stats, periodic log (§5).
- `EmuCapture.{h,cpp}` — capture write + read (JSON via `td/utils/JsonBuilder.h`, no new dep) (§7).

**New — target `mch-emu-bridge`** (links `mch-emu` + `ton-trace-emulator-core`):
- `EmuShadow.{h,cpp}` — `snapshot_trace(const ::Trace&)` + `decorate_sink(...)`. **The only TU that
  includes `TraceEmulator.h`**; kept separate so the harness can link `mch-emu` for replay without
  dragging `emulator_static` in.

**Modified** (all surgical, listed with the whole intended diff):
- `tondb-scanner/src/DataParser.{h,cpp}` — §2 seam (~20 lines, zero behavior change).
- `ton-trace-emulator/src/TraceScheduler.h` — 1 include, 1 ctor param, 2 members.
- `ton-trace-emulator/src/TraceScheduler.cpp` — 3 lines in the ctor (decoration).
- `ton-trace-emulator/src/TraceInserter.h` — `NullTraceInsertManager` (~10 lines, §6 classify-only).
- `ton-trace-emulator/src/main.cpp` — 3 flags + startup validation (§6).
- `ton-trace-emulator/CMakeLists.txt`, `ton-mch-engine/CMakeLists.txt` — §8.

**Untouched**: `TraceInserter.cpp`, `Serializer.hpp`, `TraceEmulator.{h,cpp}`, `BlockEmulator.*`,
listeners, `mch-classify` sources, `Value.h` (no new VType — the adapter reuses existing kinds, so
the append-only rule and its clean-rebuild hazard never come into play).

## 2. Converter seam — DECISION: reuse `ParseQuery`'s parsers, do NOT retarget `Serializer.hpp`

Chosen: a **static-visibility seam in `DataParser`**, because (1) `SchemaTraceLoader` is gated (147
fixtures, diff 0) against exactly the `schema::Transaction` shape `ParseQuery` produces — a second
parser is a second thing to keep true; (2) retargeting `Serializer.hpp::parse_tx` (`:533`) means
rewriting its message parsing anyway (emulator `Message.value` is `optional<uint64_t>` grams-only
`:151` — the exact §2.2 extra-currency loss we go in-process to avoid — and `schema::TransactionDescr`
carries richer phase structs), i.e. ~350 duplicated lines vs ~20 mechanical ones; (3) `parse_message`
is the only member with state, `mc_block_.config_->get_global_version()` (`DataParser.cpp:262`) —
everything it and `process_transaction_descr` call is already pure.

Seam (`DataParser.h`), definitions in `.cpp` keep their `ParseQuery::` qualification:

```cpp
 public:  // EM1 seam: pure TLB->schema parsers, reusable off-pipeline.
  static td::Result<schema::Message> parse_message(td::Ref<vm::Cell> msg_cell, int global_version);
  static td::Result<schema::TransactionDescr> process_transaction_descr(vm::CellSlice& td_cs);
  static td::Result<schema::Transaction> parse_transaction(td::Ref<vm::Cell> tx_root,
                                                           ton::WorkchainId workchain,
                                                           int global_version);
 private:  // + `static` on the 7 phase parsers (:27-33) — a static caller needs static callees.
```

`.cpp` changes: (a) `parse_message`'s config read (`:262`) becomes the `global_version` parameter;
(b) new `parse_transaction` = today's `parse_transactions` inner body (`:642-681`) minus
`mc_seqno`/`account_states`, account from `trans.account_addr`; (c) `parse_transactions` (`:596`)
delegates to it, then re-assigns `schema_tx.account = block::StdAddress(blk_id.id.workchain,
cur_addr)` + `schema_tx.mc_seqno = mc_seqno_` and keeps the `account_states` bookkeeping — provably
identical output to today.

```cpp
// EmuTraceConvert.h — per node: ParseQuery::parse_transaction(n.tx_root, n.address.workchain, gv),
// then tx.mc_seqno = n.mc_seqno (the wrapper-node field, plan §2.1). ANY node failing => whole-trace
// error (a partial tree classifies wrong, never silently).
td::Result<std::vector<schema::Transaction>> emu_to_schema_txs(const EmuTraceSnapshot &snap, int gv);
```

`global_version`: `kEmuGlobalVersion = 12` (current mainnet semantics). It selects `extra_flags` vs
legacy `ihr_fee` in `int_msg_info` — **neither field is read by the engine**
(`SchemaTraceLoader.cpp:66-96` maps only hash/source/destination/opcode/value/created_lt/created_at/
bounce/bounced/body_boc), so the choice cannot affect classification. Q1.

## 3. Snapshot + sink decoration

`EmuTypes.h` (emulator-header-free on purpose — `EmuFinality` mirrors `FinalityState` so the actor,
the converter and the capture format never include `TraceEmulator.h`):

```cpp
enum class EmuFinality : std::uint8_t { emulated = 0, confirmed = 1, finalized = 2 };
struct EmuTxRec { block::StdAddress address; td::BufferSlice tx_boc;  // SERIALIZED, not a Ref — below
                  std::uint32_t mc_seqno{0}; EmuFinality finality{EmuFinality::emulated}; };
struct EmuTraceSnapshot {
  std::string trace_id;        // base64(ext_in_msg_hash_norm) == the Redis trace key (TraceInserter.cpp:54)
  bool tx_limit_exceeded{false};
  std::vector<EmuTxRec> nodes; // PRE-ORDER FLATTEN — to_tree relinks by message hash, tree not needed
  ParsedBlockLookupSource::InterfaceMap interfaces;  // already V2-adapted (§4)
};
```

`EmuShadow.h` (bridge):

```cpp
td::Result<EmuTraceSnapshot> snapshot_trace(const ::Trace &trace);  // NB: ::Trace — mch::Trace exists too
std::function<void(::Trace, td::Promise<td::Unit>, MeasurementPtr)> decorate_sink(
    std::function<void(::Trace, td::Promise<td::Unit>, MeasurementPtr)> inner,
    td::actor::ActorId<EmuClassifierActor> classifier,
    std::shared_ptr<std::atomic<std::size_t>> pending);
```

**The snapshot carries BOC BYTES, not `Ref<vm::Cell>`** — the one thing it may not defer. An emulator
tx cell can be lazily backed by the block's cell store (`StaticBagOfCellsDb`), which the emulator frees
once the sink returns, while the shadow classifier reads the snapshot on a later actor turn: refs made
~2% of live emissions die in the converter with *"StaticBocDb is already destroyed, cannot fetch cell"*.
Serializing inside `snapshot_trace` — `vm::std_boc_serialize(root, vm::BagOfCells::Mode::WithCRC32C)`,
the same call/mode the emulator's own Redis path uses via `convert::to_bytes` (`Serializer.hpp:255`),
so the bytes are the ones it already produces for these cells — decouples the snapshot from emulator
lifetimes entirely and unifies the three inputs: live, capture and replay all start from the same
bytes (the converter deserializes once, §2; the capture only base64s them, §7 — format unchanged).

This ADDS A FAILURE AND THROW SOURCE ON THE EMULATOR'S OWN STACK (walking cells can fail or raise
`vm::VmError`, which is not a `std::exception`), so `decorate_sink` wraps snapshot+send in a
`Result` + try/catch that lets nothing escape into the emulator. On failure it rolls back the
`pending` increment exactly as the drop path does — the `classify` that would have decremented never
runs — logs `SNAPSHOT FAILED`, and bumps a SEPARATE `snapshot_failed` counter (§5): `dropped` means
backpressure and must stay readable as such. Interface source = `trace.interfaces` (trace-final, incl.
emulated accounts — the map that goes to Redis, `TraceInserter.cpp:160`), not `committed_interfaces`;
the adapter drops `content` metadata maps, so it is cheaper than copying the detector map.
`TraceScheduler.h` ctor gains a 9th param `mch::EmuShadowConfig mch_shadow` (a struct,
so later knobs need no signature churn); ctor body, after the existing `insert_trace_` assignment:

```cpp
if (mch_shadow_.prep) {
  mch_classifier_ = td::actor::create_actor<mch::EmuClassifierActor>("MchEmuClassifier", mch_shadow_);
  insert_trace_ = mch::decorate_sink(std::move(insert_trace_), mch_classifier_.get(), mch_shadow_.pending);
}
```

Must be the CTOR, not a setter: `start_up()` (`:135`) copies `insert_trace_` **by value** into the
listeners, and a `send_closure`d setter would land after `start_up`. Same placement precedent as
`invalidated_trace_tracker_` (`:113`).

**Backpressure**: `pending` is a shared atomic incremented in `decorate_sink` and decremented at the
end of `classify`. Over `kMaxPendingSnapshots = 1024` the decorator DROPS the snapshot and bumps a
`dropped` counter (stats §5) — a shadow feature must not grow the emulator's memory without bound.

## 4. `DetectedInterface` → `BlockchainInterfaceV2`

Detector `Result`s are field PREFIXES of the V2 structs; the tail (`last_transaction_lt/now`,
`code_hash`, `data_hash`) is never read by `ParsedBlockLookupSource::tier1` and stays
value-initialized. `EmuInterfaces.h`:

```cpp
schema::JettonWalletDataV2         to_v2(const JettonWalletDetectorR::Result &r);
schema::NFTItemDataV2              to_v2(const NftItemDetectorR::Result &r);   // content dropped
schema::GetGemsNftFixPriceSaleData to_v2(const GetGemsNftFixPriceSale::Result &r);
schema::GetGemsNftAuctionData      to_v2(const GetGemsNftAuction::Result &r);
```

`EmuShadow.cpp` visits `Trace::Detector::DetectedInterface` (`td::overloaded`) and drops the two
unused arms: `JettonMasterDetectorR` / `NftCollectionDetectorR` have no tier-1 kind (those are
jetton_wallet / nft_item / nft_sale / nft_auction / dedust_pool, `ParsedBlockLookupSource.cpp:59-128`);
adding one later is two lines. **`dedust_pool` has no emulator detector** (plan §2.4): DeDust lookups
miss here in EM1 and land in the `misses` counter — expected, not a bug; fixed in EM3. Lookup source =
`ParsedBlockLookupSource(&snap.interfaces)` with **no tier-2 hook** (no DB reads on any path).

## 5. Classifier actor + stats

```cpp
struct EmuShadowConfig {
  std::shared_ptr<const MchEnginePrep> prep;   // nullptr = feature off
  std::string capture_dir;                     // empty = no capture
  int global_version{kEmuGlobalVersion};
  std::shared_ptr<std::atomic<std::size_t>> pending;
};
class EmuClassifierActor : public td::actor::Actor {
 public:
  explicit EmuClassifierActor(EmuShadowConfig cfg);
  void classify(EmuTraceSnapshot snap);  // fire-and-forget entry, never throws
  void start_up() override;              // arms the stats alarm
  void alarm() override;                 // periodic stats line, re-arms
};
```

`classify` mirrors `MchClassifierActor::classify_one` (`MchClassifierActor.cpp:42-89`) — same
try/catch, same `FailureCategory` mapping — with `emu_to_schema_txs` in front and no per-block tx
grouping (one snapshot == one trace). Reused verbatim from Phase G: `MchEnginePrep`,
`load_engine_prep`, `FailureCategory`, `LookupStats`, `schema_to_trace`, `to_tree`/`init_block`,
`classify_trace`.

```cpp
struct EmuShadowStats {              // logged every kStatsIntervalSec = 10s, cumulative + delta
  std::size_t emissions{0}, reemissions{0}, dropped{0}, snapshot_failed{0};  // dropped = backpressure
  std::size_t classified{0}, failed{0}, convert_failed{0}, tx_limit_exceeded{0}, actions{0};
  std::array<std::size_t, 3> by_finality{};        // root-node EmuFinality of the emission
  std::map<FailureCategory, std::size_t> by_category;
  ParsedBlockLookupSource::LookupStats lookups;    // tier1 / tier2 / miss
};
```

**Re-emissions** (2 without `--db-event-fifo`, 3 with — plan §3): every emission IS classified (the tx
set can change between emulated and finalized, and that diff is what shadow mode exists to see), but
attributed — a bounded LRU of seen `trace_id`s (`kMaxSeenTraces = 65536`, mirroring
`kMaxSeenSignedBlocks`, `TraceScheduler.cpp:22`) splits `emissions` into first-seen vs `reemissions`,
and `by_finality` shows the mix. `tx_limit_exceeded` traces are classified with the flag counted (plan
§2.6: never silent). Log prefix `[mch-emu]`, distinct from postgres's `[mch-shadow]`. Health-hash
piggyback (`TraceScheduler.cpp:431-448`) deferred — it would pull `sw::redis` into mch for telemetry
only. Q3.

## 6. CLI (`ton-trace-emulator/src/main.cpp`)

- `--mch-artifact <mch_ir.json>` — enables shadow classification. Absent ⇒ `EmuShadowConfig.prep ==
  nullptr` ⇒ ctor skips decoration ⇒ **zero behavior change, zero added work per trace**.
- `--mch-capture <dir>` — requires `--mch-artifact` (else startup error). §7.
- `--mch-classify-only` — requires `--mch-artifact`; swaps `RedisInsertManager` for
  `NullTraceInsertManager` (new ~10-line header-only class next to it in `TraceInserter.h`: resolves
  the promise, writes nothing). Removes only the per-trace Redis WRITE path; the input-channel
  subscription, `health:` hash and `InvalidatedTraceTracker` are scheduler-owned and untouched.

Startup validation mirrors `ton-index-postgres/src/main.cpp:629-637` — `mch::load_engine_prep(path)`
before the scheduler runs, `LOG(FATAL)` on error (prod-design §4: artifact and binary ship together,
a mismatch is a build bug), then `LOG(INFO) << "MCH emu shadow ENABLED (artifact …)"`.

## 7. Capture + replay

`--mch-capture <dir>` writes one JSON per classified emission,
`<dir>/<trace_id_urlsafe>.<finality>.json`, dumping the CONVERTER INPUTS (not the outputs):

```json
{"trace_id":"…","tx_limit_exceeded":false,"global_version":12,
 "nodes":[{"account":"0:AB…","mc_seqno":123,"finality":"emulated","tx_boc":"te6…"}],
 "interfaces":{"0:AB…":{"jetton_wallet":{"balance":"…","address":"…","owner":"…","jetton":"…"}}}}
```

Replay (OPT for EM1): harness mode `ton-mch-engine --classify-emu <capture.json>` — `EmuCapture::read`
rebuilds the snapshot, the SAME `emu_to_schema_txs` path runs, rendered through the existing
`ClassifyDump` adapter so output is directly diffable against the Python twins. Harness links
`mch-emu` only; no emulator, no DB, no Redis. JSON rather than the msgpack fixture format is
deliberate: a fixture writer must reproduce the Python recorder's key layout byte-for-byte —
EM2-grade work, zero EM1 benefit.

## 8. Build wiring

```cmake
# ton-mch-engine/CMakeLists.txt
add_library(mch-emu STATIC src/emu/EmuTraceConvert.cpp src/emu/EmuInterfaces.cpp
                           src/emu/EmuClassifierActor.cpp src/emu/EmuCapture.cpp)
target_include_directories(mch-emu PUBLIC src)
target_link_libraries(mch-emu PUBLIC mch-classify)          # -> tondb-scanner, tdactor, ton-abi
add_library(mch-emu-bridge STATIC src/emu/EmuShadow.cpp)
target_link_libraries(mch-emu-bridge PUBLIC mch-emu ton-trace-emulator-core)
# ton-trace-emulator/CMakeLists.txt
target_link_libraries(ton-trace-emulator ton-trace-emulator-core mch-emu-bridge redis++_static msgpack-cxx)
```

`ton-mch-engine` is `add_subdirectory`'d at root `:103`, before `ton-trace-emulator` at `:110` —
naming `ton-trace-emulator-core` on the right-hand side before its definition is fine (link libraries
resolve at generate time). No cycle: the core lib knows nothing of mch. The emulator inherits
`ton-abi` transitively through `mch-classify` (expected; plan §3).

## 9. Verification — COMPILE ONLY

Done for EM1 = (1) `cmake --build <linux-build-dir> --target mch-classify mch-emu mch-emu-bridge
ton-mch-engine ton-trace-emulator` all green; (2) gate binary `ton-mch-engine` still builds with its
ctest wiring unchanged (no test is RUN this phase); (3) `ton-index-postgres` still links
`mch-classify` unchanged — the `DataParser` seam is the only shared edit and is behavior-preserving
by construction.

No emulator launch, no Redis, no node DB, not even a `--help` smoke — real testing is a later phase on
the remote server (plan §0). Expect at step 1: the emulator target has never been built in our env;
fresh LP64/clang findings there are pre-existing, not EM1 regressions — report, don't absorb.

## 10. Open questions for the lead

- **Q1 `global_version`**: ship the constant `12` (§2 — the two affected fields never reach the
  engine), or wire the live `latest_config_` value (`TraceScheduler.cpp:229`) through a setter?
  ► constant; revisit only if EM2 ever serializes those fields.
- **Q2 classify-only**: keep in EM1 (costs the 10-line `NullTraceInsertManager`) or drop? ► keep — it
  is what makes the first real run risk-free for production pending data.
- **Q3 health piggyback**: log-only (►), or accept `sw::redis` in `mch-emu` to publish shadow counters
  into `health:ton-trace-emulator`?
- **Q4 replay tool** (§7 OPT): this wave or a follow-on? Capture itself stays in EM1 either way.
- **Q5 tondb-scanner edit**: confirm we may touch `DataParser.{h,cpp}` — a second upstream component
  beyond the emulator. The §2 alternative avoids it at the cost of ~350 duplicated parser lines.

---

# EM1.5 — full integration: no snapshot, cell anchor, request/response (design)

> Status: DESIGN, awaiting lead ack. Supersedes EM1 §3 and §5's entry point; everything else in EM1
> (converter seam §2, V2 adapter §4, CLI §6, capture §7, build wiring §8) is KEPT. Verification is
> again COMPILE ONLY (§19). Baseline = commit `6c6f3f4` on merge-v13.

## 11. Scope

Three changes, in dependency order:

1. **Cell lifetime by anchor, not by copy.** `EmuTraceSnapshot`'s per-tx `td::BufferSlice tx_boc`
   (BOC serialization on the emulator's own stack) is replaced by the live `td::Ref<vm::Cell>` plus
   an explicit **anchor** that keeps the backing cell store alive. §12 identifies the anchor.
2. **Request/response.** The sink sends `(view, td::Promise<EmuClassifyResult>)` and receives Action
   rows + failure/stats back; the anchor rides inside the request and comes back inside the result,
   so every cell any returned value can still reference stays loadable at the integration point.
3. **Insert gates on the response** (EM2 sequencing, one wave early), bounded by an admission cap, a
   per-request deadline and a sticky fail-open watchdog (§16) so a classifier stall degrades to "no
   classification", never to "no insertion".

Non-goals: writing actions anywhere (EM2), tier-2 lookups (plan §1.4b), DedustPoolDetector (EM3).

## 12. Cell-lifetime analysis — WHAT owns the cells behind `TraceNode.transaction_root`

**Answer: the block's `td::Ref<ton::validator::BlockData>` (concrete `BlockQ`), one per shard block,
and nothing else.** Chain of evidence:

- `BlockQ::init()` (`external/ton/validator/impl/block.cpp:45,49,61`) deserializes the block file
  **lazily**: `vm::StaticBagOfCellsDbLazy::create(BufferSliceBlobView(data_.clone()))` → held in the
  member `boc_` (`block.hpp:33`); the root cell is `boc_->get_root_cell(0)` → `root_` (`block.hpp:32`,
  returned by `root_cell()` `:51`).
- That root is a `StaticBocRootCell = RootCell<std::shared_ptr<StaticBagOfCellsDb>>`
  (`crypto/vm/db/StaticBagOfCellsDb.cpp:156`, created `:546`) whose `extra_` (`:73`) holds the boc
  **strongly**. Every INTERIOR cell is a `StaticBocExtCell` whose extra is
  `{int idx; std::weak_ptr<StaticBagOfCellsDb> deserializer;}` (`:140-142`) — **weak**. When the
  weak pointer expires the loader returns exactly the observed error, `:150`
  *"StaticBocDb is already destroyed, cannot fetch cell"*.
- Transaction cells are interior cells: `BlockParser::start_up` unpacks `block_data_->root_cell()`
  into an `AugmentedDictionary` and stores `tx_info.root = tvalue`
  (`ton-trace-emulator/src/BlockEmulator.cpp:52,58,101`; out-msg cells likewise `:129-130`). They are
  copied verbatim into the trace: `trace_node->transaction_root = tx.root` (`:237` finalized, `:591`
  confirmed). **A `td::Ref<vm::Cell>` to a transaction therefore keeps NOTHING alive** — copying Refs
  (EM1's pre-Option-B attempt) cannot fix the lifetime; only holding a strong handle to the boc can.
- Who holds it and when it dies: `McBlockEmulator::mc_data_state_` (member, `BlockEmulator.h:49`,
  moved in at `.cpp:153`) and `ConfirmedBlockEmulator::block_data_state_` (`BlockEmulator.h:96`).
  Both actors `stop()` from their completion path — `finish_block_if_done` (`:433-442`, `stop()`
  `:441`) and `trace_finished` (`:718-733`, `stop()` `:731`) — which runs once every trace's insert
  promise has resolved. Actor death destroys the member, drops the last `BlockQ` ref, frees the boc.
  **This is precisely the EM1 bug**: fire-and-forget classification runs on the classifier actor's
  turn, which can be after the last promise resolved, i.e. after the boc is gone (~2% of emissions).
- **What does NOT need an anchor.** (a) Emulator-produced tx cells are ordinary in-memory `DataCell`s.
  (b) Account/state-derived cells come from celldb: `RootDb::get_block_state_root` →
  `CellDb::load_cell` (`external/ton/validator/db/rootdb.cpp:289,303`), and dynamic-boc ext cells
  hold `std::shared_ptr<CellDbReader>` **strongly** (`crypto/vm/db/DynamicBagOfCellsDb.cpp:35-36`),
  with the CellDb actor alive for the process. (c) Listener-path traces (RedisListener /
  OverlayListener) parse the ext-in message from Redis bytes and emulate everything else, so they
  touch no block boc at all — their anchor is legitimately empty.
- **Concurrency is not a hazard**: `ExtCell` caches its loaded `DataCell` in a `td::AtomicRef`
  (`crypto/vm/cells/ExtCell.h:74-75`) and the lazy boc guards its index/cache with mutexes, so the
  classifier walking the same cells on another thread while `TraceInserter` walks them is safe;
  `td::Ref` refcounts are `std::atomic<int>` (`crypto/common/refcnt.hpp:36`).

So the minimal correct handle is **the set of block root cells** (`block_data->root_cell()`) for
every shard block whose transactions can appear in the trace: `mc_data_state_.shard_blocks_diff_`
for the finalized path (the only blocks `BlockParser` runs over, `BlockEmulator.cpp:169-180`) and
`block_data_state_` for the confirmed path. Holding the root cell is equivalent to holding the
`BlockData` — same `shared_ptr<StaticBagOfCellsDb>` — and keeps `vm` as the only dependency, so no
validator header enters mch. Cost per trace: one `shared_ptr` copy (the anchor is built ONCE per
block and shared by all its traces).

## 13. Integration point — CHOSEN: the sink decorator, with the anchor carried on `Trace`

**Chosen (a+):** keep EM1's single decoration of `insert_trace_` in the `TraceEmulatorScheduler` ctor
(`TraceScheduler.h:118-121`), and give `Trace` one new member so the sink has the lifetime handle it
structurally lacks today:

```cpp
// TraceEmulator.h, struct Trace — no mch dependency, vm::Cell is already included there.
// Keeps the block cell store (StaticBagOfCellsDbLazy behind BlockData) alive for as long as this
// trace exists; empty for listener-emulated traces, which reference no block boc.
std::shared_ptr<const std::vector<td::Ref<vm::Cell>>> cell_anchor;
```

set once per block in each block emulator (`McBlockEmulator::start_up` builds it from
`mc_data_state_.shard_blocks_diff_`; `ConfirmedBlockEmulator::start_up` from `block_data_state_`) and
assigned in `trace_emulated` before `trace_processor_(...)` — 3 lines per class. Why this wins:

- **Lifetime naturalness.** The anchor travels with the data it protects. Any future trace source
  gets the invariant for free, and the sink — which today receives a `Trace` whose cells may already
  be dangling-on-a-timer — becomes self-sufficient. It also makes explicit an invariant upstream
  relies on implicitly (the block actor happens to outlive the insert because it waits on the
  promise); that is arguably an upstream-mergeable robustness fix, not just our scaffolding.
- **Surgical diff.** One member in `TraceEmulator.h`, 6 lines in `BlockEmulator.{h,cpp}`, zero new
  call-site plumbing: the ctor decoration, the 4 trace sources and the `insert_trace_` signature all
  stay exactly as they are.
- **EM2 readiness.** The decorator sits immediately upstream of
  `send_closure(insert_manager, &ITraceInsertManager::insert, ...)`, i.e. the one choke point every
  source funnels through. EM2 widens `insert()` by one parameter and moves the already-computed rows
  into it; nothing else moves.

**Rejected — (a) EM1's decorator as-is (no anchor).** The sink signature
`void(Trace, Promise<Unit>, MeasurementPtr)` carries no lifetime handle, so the decorator's only
options are to serialize (EM1 Option B, the thing being removed) or to rely on the promise chain
alone. Relying on the chain is *almost* right — the block actor does live until the insert promise
resolves — but it collapses on exactly the paths the failure contract needs (§16): shed, deadline
and fail-open all release the insert while a request may still be in the classifier. Without an
anchor those degrade back to "StaticBocDb is already destroyed". Rejecting the anchor means
rejecting the timeout contract.

**Rejected — (b) inside `BlockEmulator`'s per-trace processing.** `mc_data_state_` is right there, so
lifetime is trivial, but it covers only 2 of 4 sources: the listener paths (RedisListener,
OverlayListener) are where the *pending* traces come from — the exact traffic Python's
`pendings.py` classifies today and that EM2 must replace. Wiring all four means the classifier ActorId
and mch types entering `BlockEmulator.{h,cpp}`, `RedisListener.cpp` and `OverlayListener.cpp` — four
upstream files instead of one, four copies of the admission/deadline logic, and a much worse merge
against upstream v1.3.

**Rejected — (c) the `TraceInserter` / insert path.** Attractive because it is where the MULTI is
built and where the per-trace promise already lives, and it would need no `Trace` change if the
anchor were passed alongside. But `TraceInserter::start_up` is a *blocking* actor body — it issues
synchronous `hlen`/`hget` round-trips and `exec()` inline (`TraceInserter.cpp:56,291,326,260`) — so
classification would either run inline on that thread (a second serialization point in front of every
Redis transaction, and mch linked into the insert TU) or need a nested request/response inside an
actor that already owns a promise, i.e. strictly more machinery than the decorator for the same
result. Worse, it is the wrong side of `--mch-classify-only`: with `NullTraceInsertManager` swapped
in (EM1 §6), a classifier living in the inserter disappears together with the Redis writes, killing
the risk-free first live run. EM2 still lands the rows here — via the widened `insert()` — which is
what "structurally ready" means; it does not require classification to live here.

**Also considered — per-block decoration in the scheduler** (`TraceScheduler.cpp:274` finalized,
`:380` confirmed), capturing the anchor in the closure like the existing
`make_signed_trace_processor` (`:407`). Confines every edit to `TraceScheduler.cpp` (~12 lines, zero
edits to `TraceEmulator.h`/`BlockEmulator.*`), but needs two decoration flavours (anchored for block
sources, anchorless for listeners), keeps both a raw and a decorated sink as members, and re-derives
the anchor from a side channel that a new trace source can silently forget. Offered as **Q6** if the
lead prefers zero edits in the emulator core; the anchor type and everything downstream are identical.

NB `make_finalized_trace_processor` (`:414-429`) is dead code with zero callers (plan §1) — EM1.5
neither revives nor deletes it. Reviving it would start emitting
`InvalidatedTraceTracker::add_finalized_trace` calls that do not happen today; that is a behavior
change and must not ride along with this wave.

## 14. Data flow

```
McBlockEmulator / ConfirmedBlockEmulator                    RedisListener / OverlayListener
  start_up: anchor = {block_data->root_cell(), …}             (no anchor — celldb/in-memory cells)
  trace_emulated: trace.cell_anchor = anchor            \    /
                                                         \  /
                    insert_trace_  (decorated once in the TraceScheduler ctor)
                                   │
        ┌──────────────────────────┴───────────────────────────────────────────┐
        │ gate: disabled? in_flight >= cap?   ── yes ─> note_shed → inner(trace, promise, m)
        │ no:  view = make_view(trace)   [Refs + PODs + V2 ifaces; NO cell is loaded]
        │      view.anchor = trace.cell_anchor;  view.deadline = now + 200ms
        │      send_closure(classifier, &EmuClassifierActor::classify, view, P)
        └──────────────────────────┬───────────────────────────────────────────┘
                                   │  (classifier actor turn, own thread)
        past deadline? ── yes ─> EmuClassifyResult{outcome=shed_deadline}
        no: [capture: std_boc_serialize here, --mch-capture only]
            ParseQuery::parse_transaction(tx_root) ×N  → schema::Transaction
            schema_to_trace → to_tree/init_block → classify_trace(ParsedBlockLookupSource)
            build_action(produced) ×N                  → std::vector<mch::Action>
            stats += …                                 → EmuClassifyResult{actions, anchor, …}
                                   │  P.set_value(result)
        ┌──────────────────────────┴───────────────────────────────────────────┐
        │ continuation (classifier thread): result LOGGED/COUNTED (EM1.5)      │
        │                                   ── EM2: moved into insert()        │
        │ inner(std::move(trace), std::move(promise), measurement)             │
        └──────────────────────────────────────────────────────────────────────┘
                                   │
                    ITraceInsertManager::insert → TraceInserter (MULTI, unchanged)
                                   │ promise resolves
                    block emulator's trace_finished → … → stop()  (anchor may outlive it)
```

The emulator's own stack now does: one tree walk copying `td::Ref`s, one interface-map adapt, one
`shared_ptr` copy, one `send_closure`. No cell is loaded, so — unlike EM1 §3 — **the decorator can no
longer throw `vm::VmError` and cannot fail**; the try/catch and the `snapshot_failed` counter move
out of the emulator's stack entirely (they stay inside the actor, around the parse).

## 15. Types and signatures

```cpp
// EmuTypes.h — still emulator-header-free.
using EmuCellAnchor = std::shared_ptr<const std::vector<td::Ref<vm::Cell>>>;  // §12; may be null

struct EmuTxRef {                       // was EmuTxRec
  block::StdAddress address;
  td::Ref<vm::Cell> tx_root;            // was td::BufferSlice tx_boc — LIVE cell, anchor-backed
  std::uint32_t mc_seqno{0};
  EmuFinality finality{EmuFinality::emulated};
};

struct EmuTraceView {                   // was EmuTraceSnapshot — a VIEW + a lifetime handle
  std::string trace_id;
  bool tx_limit_exceeded{false};
  std::vector<EmuTxRef> nodes;                       // pre-order flatten (unchanged rationale)
  ParsedBlockLookupSource::InterfaceMap interfaces;  // already V2-adapted (EM1 §4)
  EmuCellAnchor anchor;                              // keeps `tx_root`s loadable, §12
  std::int64_t sent_us{0}, deadline_us{0};           // emu_now_us() basis, §16
};

enum class EmuClassifyOutcome : std::uint8_t {
  classified, classify_failed, convert_failed,     // the classifier ran
  shed_admission, shed_deadline, bypassed_disabled // it did not (§16)
};

struct EmuClassifyResult {
  std::string trace_id;
  EmuClassifyOutcome outcome{EmuClassifyOutcome::bypassed_disabled};
  EmuCellAnchor anchor;               // HANDED BACK — see the Value hazard below.
                                      // DECLARED BEFORE `actions`: reverse-order
                                      // destruction, so the rows die first.
  std::vector<Action> actions;        // ActionBuild.h rows; EM2 serializes these into the MULTI
  bool used_fallback{false};          // rows came from ClassifyResult::fallback_actions
  std::size_t unported_btypes{0};     // produced blocks build_action() declined
  bool failure{false};
  std::string failure_reason;
  FailureCategory failure_category{FailureCategory::none};
  ParsedBlockLookupSource::LookupStats lookup_stats;
  std::int64_t queue_us{0}, classify_us{0};
};
```

`sent_us`/`deadline_us` are stamped by `emu_now_us()` — `td::Clocks::monotonic()` scaled to
microseconds, NOT `td::Clocks::system()`: the stamp is written on an emulator thread and compared on
the classifier's, so a wall-clock step (NTP) must not be able to expire or extend a deadline.
`sent_us` is what makes `queue_us` measurable at the dequeue.

```cpp
// EmuClassifierActor.h
void classify(EmuTraceView view, td::Promise<EmuClassifyResult> promise);  // never throws
void note_shed(EmuClassifyOutcome why);                                    // gate-side counters

// EmuShadow.h (bridge)
EmuTraceView make_view(const ::Trace &trace);   // pure Ref/POD copy; cannot fail, cannot throw
EmuTraceSink decorate_sink(EmuTraceSink inner, td::actor::ActorId<EmuClassifierActor> classifier,
                           std::shared_ptr<EmuGate> gate, bool nonblocking);  // §16.6
```

**Why `mch::Action` and not `CoreAction`:** `CoreAction` is raw `Block*` into the classifier's
`TraceContext` arena (`ClassifyCore.h:48-53`) and dies with it; `mch::Action` (`ActionBuild.h:19-33`)
is value-semantic. `build_action(Block*, Action&)` runs inside `classify`, while the arena is alive.

**Value hazard (design finding, needs a guard).** `Value` is not unconditionally self-contained
(`Value.h:47-60`): `VType::Cell` carries a `td::Ref<vm::Cell>` — which for a payload cell taken off
a block transaction is exactly an anchor-backed `StaticBocExtCell` — and `VType::Block` carries a raw
arena `Block*`. Hence (a) the result carries the anchor back, so a `Cell` value inside a returned
Action stays loadable at the integration point and, in EM2, through msgpack serialization; and (b)
the actor runs a shallow `scrub_arena_refs(Action&)` after `build_action` that nulls any
`VType::Block` value and counts it (`arena_refs_scrubbed`, logged once at WARNING). A Block-typed
value reaching an Action is believed impossible — `ActionBuild.cpp`'s fills go through
`av_addr`/`av_amount`/`dg` — but "believed impossible" and "use-after-free in production" are one
audit apart; the scrub converts it into a counter. ~15 lines.

## 16. Failure / timeout contract

Shared gate state (replaces the bare `pending` atomic; lives in `EmuShadowConfig`, touched by the
decorator on the emulator's threads and by the actor on its own):

```cpp
struct EmuGate {
  std::atomic<std::size_t> in_flight{0};
  std::atomic<std::int64_t> last_response_us{emu_now_us()};  // seeded, not 0 — see §16.3
  std::atomic<bool> disabled{false};   // sticky fail-open
};
inline constexpr std::size_t kMaxInFlightClassify = 1024;   // was kMaxPendingSnapshots
inline constexpr std::int64_t kClassifyDeadlineUs = 200'000;
inline constexpr std::int64_t kStallFailOpenUs   = 5'000'000;
```

1. **Admission cap.** `in_flight >= kMaxInFlightClassify` ⇒ do not send; `outcome=shed_admission`;
   insert proceeds immediately. Bounds memory and queue depth. Expected in-flight under live load is
   ~60-250 (§18), so the cap is a valve, not a working limit.
2. **Deadline.** Checked by the actor when it dequeues, before any work: past `deadline_us` ⇒
   `shed_deadline`, respond immediately. This is the backlog valve — a burst sheds instead of piling
   on. Note precisely what it bounds: the 200 ms caps the QUEUE WAIT only, since it is tested once,
   at dequeue, and never again while the trace is being classified. Total added insert latency =
   queue wait (≤200 ms) + classify time, and classify time has no bound of its own — a single
   pathological trace can exceed 200 ms all by itself. That residue is exactly what §16.3's watchdog
   exists for; it is not covered here.
3. **Sticky fail-open watchdog.** The decorator checks, per call:
   `in_flight >= cap && now - last_response_us > kStallFailOpenUs` ⇒ set `disabled` (permanent,
   `LOG(ERROR) "[mch-emu] classifier stalled, shadow classification DISABLED"`), and from then on
   every trace takes the `bypassed_disabled` path. `last_response_us` is SEEDED at gate construction
   rather than left at 0, so staleness is measured from process start and a classifier that wedges on
   its very first request trips this too. This covers the only stall the first two rules
   cannot: a non-terminating loop inside the engine, which would wedge the classifier's thread
   forever. **Residual risk, stated plainly:** the ≤1024 requests already in flight at that instant
   never get a response, so their inserts never fire and their block emulators never complete. Making
   even those safe requires a per-request timer (a gate actor with a deadline heap, ~80 lines and a
   hop per trace) — proposed as **Q7**, recommended NO for EM1.5: the classifier does no I/O on this
   path (tier-2 is unwired) and the 165-fixture gate exercises the same engine, so a hang means an
   engine bug, and the fail-open flag plus the 5 s alarm makes it loud and self-limiting.
4. **Classifier-side failures are not gate failures.** Convert error, engine fault, malformed trace ⇒
   a normal response with `failure=true`, counters as EM1, insert proceeds. Log lines unchanged.
5. **Invariant.** Every path — shed, deadline, disabled, failure, success — calls `inner(...)`
   exactly once, and every send is answered exactly once. `in_flight` is decremented in the response
   continuation only (single place), so backpressure cannot leak.
6. **`--mch-classify-nonblocking`** (new flag, ~5 lines): calls `inner(...)` immediately and uses the
   response for stats only. Not the default, but it makes the first live run's blast radius provably
   zero, and it is the mode the anchor was designed for (a fire-and-forget response with correct
   cells). **Q8**: ship it or drop it.

## 17. What EM1 code is deleted / changed / kept

DELETED:
- `vm::std_boc_serialize` on the sink path (`EmuShadow.cpp:49-51`) and the whole try/catch +
  `note_snapshot_failed` + `snapshot_failed` counter (`EmuShadow.cpp:96-119`,
  `EmuClassifierActor.{h,cpp}`): with no cell loaded on the emulator's stack, there is nothing to
  catch. The `vm::VmError` catch is NOT lost — it moves into the actor, around the parse, where EM1
  already has one (`EmuClassifierActor.cpp:174-180`).
- `vm::std_boc_deserialize` in `emu_to_schema_txs` (`EmuTraceConvert.cpp:18-19`): the `Ref` goes
  straight into `ParseQuery::parse_transaction`, whose signature already takes one. The DataParser
  seam (EM1 §2) is untouched.
- `EmuTxRec::tx_boc`, `EmuTraceSnapshot`, `kMaxPendingSnapshots`, the bare `pending` atomic.

CHANGED: `snapshot_trace` → `make_view` (same walk, Refs instead of bytes, no `td::Result` — it
cannot fail); `decorate_sink` (gate + promise + continuation); `EmuClassifierActor::classify`
(promise, `build_action`, latency, deadline check); `write_capture` gains the
`std_boc_serialize` call it now needs (capture-only, cells are alive there — **capture JSON format
unchanged**, so any EM1 capture stays replayable).

KEPT AS-IS: `--mch-artifact` / `--mch-capture` / `--mch-classify-only` and their startup validation;
`NullTraceInsertManager`; the DataParser seam; `EmuInterfaces` V2 adapter; the seen-trace LRU and
first-seen/re-emission attribution (still the only way to read the counters — and re-emissions stay
first-class in EM2, which recomputes per emission); per-kind `misses_by_kind`; the `[mch-emu]` 10 s
line (extended with `shed{admission,deadline} disabled queue_us_max classify_us_{avg,max}`);
feature-off = byte-neutral (`cfg.prep == nullptr` ⇒ no decoration, and `Trace::cell_anchor` stays
null — one `shared_ptr` member that is never set).

## 18. Concurrency budget interaction

Gating means a trace's insert promise now resolves `queue + classify` later, and that promise is what
releases `in_progress_cnt_` → `finish_block_if_done` → the scheduler's `finalized_blocks_inflight_`
(`kMaxFinalizedBlocksInFlight = 2`) / `confirmed_blocks_inflight_` (`= 8`,
`TraceScheduler.cpp:23-24`). Numbers:

- Live-verified rate: 2271 traces / 240 s ≈ **9.5 traces/s**. Classification is measured in µs-ms;
  block emulation is 12-40 ms and the Redis insert already does several synchronous round-trips.
- One classifier actor is a single serialization point. At 9.5 traces/s × 3 ms ≈ **3% duty cycle**;
  even at a pessimistic 10 ms/trace it is ~10%. Queueing is not expected outside catch-up bursts,
  and a burst hits the deadline valve (§16.2) rather than the block budget.
- Per block: ~28 traces (2271 traces / ~80 blocks), all in flight simultaneously (the emulator spawns
  them in a loop), so classification is *concurrent across traces from the emulator's point of view*
  but *serial inside the actor*: worst added block latency ≈ 28 × classify_us. At 3 ms that is 84 ms
  against a 12-40 ms emulation — the one number that could actually matter, which is why
  `queue_us`/`classify_us` are in the result and in the stats line. If it bites, the fix is N
  classifier actors sharded by `hash(trace_id)` (the LRU then stays correct because a trace always
  lands on the same shard); `prep` is already shared const. Designed for, not built now — **Q9**.

## 19. Diffstat estimate and verification

| file | ± | what |
|---|---|---|
| `emu/EmuTypes.h` | +35/-15 | anchor, view, result, outcome, gate |
| `emu/EmuShadow.{h,cpp}` | +70/-55 | `make_view`, gated request/response decorator |
| `emu/EmuClassifierActor.{h,cpp}` | +110/-35 | promise entry, deadline, `build_action`+scrub, latency, stats |
| `emu/EmuTraceConvert.cpp` | +2/-6 | parse the Ref directly |
| `emu/EmuCapture.cpp` | +10/-4 | serialize at capture time |
| `ton-trace-emulator/src/TraceEmulator.h` | +4 | `Trace::cell_anchor` + comment |
| `ton-trace-emulator/src/BlockEmulator.{h,cpp}` | +14 | build anchor per block ×2, assign per trace ×2 |
| `ton-trace-emulator/src/main.cpp` | +8 | `--mch-classify-nonblocking` (Q8) |
| `ton-trace-emulator/src/TraceScheduler.h` | +2/-2 | `EmuShadowConfig` gate member rename |

≈ **+255 / -117 across 9 files**; upstream files touched: 4 (was 4 — `TraceInserter.h` swaps out for
`TraceEmulator.h`/`BlockEmulator.*`, `DataParser.*` untouched this wave). Net library code SHRINKS
on the hot path: one serialization + one deserialization per trace removed.

Verification, COMPILE ONLY, same as EM1 §9: (1) `mch-classify mch-emu mch-emu-bridge ton-mch-engine
ton-trace-emulator` all build on the Linux build dir; (2) the gate binary still builds with its ctest
wiring unchanged (no test run this wave); (3) `ton-index-postgres` still links `mch-classify`
unchanged. Additionally (4) **feature-off byte-neutrality is a code review item, not a runtime one**:
with `prep == nullptr` the ctor does not decorate, `Trace::cell_anchor` is never assigned, and the
two block emulators build the anchor unconditionally — that last one is a real (tiny) cost with the
feature off, so either build it lazily behind `if (mch_shadow_.prep)` (needs a flag reaching
`BlockEmulator`) or accept ~10 `Ref` copies per block. **Recommend accept**: it is one refcount
increment per shard block per block, and it makes the invariant unconditional.

## 20. Open questions for the lead (EM1.5)

- **Q6 anchor plumbing**: `Trace::cell_anchor` (recommended, §13) vs per-block decoration confined to
  `TraceScheduler.cpp` (zero edits in `TraceEmulator.h`/`BlockEmulator.*`)?
- **Q7 real timer**: accept the residual "≤1024 in-flight requests never answered if the engine
  hangs" (recommended), or build the gate actor with a deadline heap (~80 lines + a hop per trace)?
- **Q8 `--mch-classify-nonblocking`**: ship the escape hatch for the first live run (recommended) or
  keep the flag surface as-is?
- **Q9 classifier sharding**: single actor now (recommended) with `queue_us`/`classify_us` telemetry
  deciding later, or N shards by `hash(trace_id)` immediately?
- **Q10 EM2 seam shape**: widen `ITraceInsertManager::insert(Trace, std::vector<Action>, Promise,
  MeasurementPtr)`, or attach the rows to `Trace` like the anchor? The first keeps `Trace` a pure
  emulator type; the second keeps the signature and every call site untouched. Not needed for
  EM1.5 — deciding now shapes where EM1.5 leaves the result.
