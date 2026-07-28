# EM1 — in-process shadow classifier in ton-trace-emulator (design)

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
