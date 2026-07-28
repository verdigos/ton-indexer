# MCH classifier × ton-trace-emulator integration (EM program)

> Self-contained fresh-session entry point for the emulator-integration work. Status: **EM1
> IMPLEMENTED 2026-07-28** (compile-verified only — NOT yet run; user runs real tests separately;
> uncommitted on merge-v13). Design: docs/em1-design.md. As-built: ton-mch-engine/src/emu/ (11
> files, targets mch-emu + mch-emu-bridge) + surgical host edits (TraceScheduler.h ctor decoration,
> main.cpp flags --mch-artifact/--mch-capture/--mch-classify-only, NullTraceInsertManager,
> DataParser static seam); adversarial review passed (1 blocker fixed: vm::VmError catch — VmError
> is NOT std::exception; per-kind miss_by_kind stats in the [mch-emu] 10s line). All facts +
> file:line refs below RE-VERIFIED against branch merge-v13 on 2026-07-28. Companion state:
> docs/ton-abi-plan.md (bridge as-built), docs/mch-cpp-plan.md §1 (gate set),
> docs/mch-cpp-prod-design.md (shadow-actor Phase G design). Next: EM1 live shadow run (user), then
> tier-2 wiring (§1.4b) before EM2.

## 0. LOCKED DIRECTION (user, 2026-07-30)

**In-process**: classifier becomes PART of ton-trace-emulator — no Redis hop between emulation and
classification. That is one of the purposes of the C++ classifier migration. Redis remains only as
the OUTPUT contract (ton-index-go pending endpoints). The out-of-process subscriber option (mirror
of Python's `--emulated-traces`) was evaluated and REJECTED as the end state — kept below only as
background.

Real-world testing runs on the remote server (archive node + work DB live there; emulator must run
as root; `ulimit -n 99999` before launch; runs indefinitely → every test run time-boxed / killable —
systemd transient units (`systemd-run -p LimitNOFILE=99999 -p RuntimeMaxSec=…`) fit all three).

## 1. Emulator facts (survey 2026-07-30, re-verified at merge-v13 2026-07-28)

> Every ref below is current at merge-v13. Correction to the old staleness note: v1.3 did NOT add an
> emulator DbEventListener — it MOVED the existing one to
> `tondb-scanner/src/DbEventListener.{h,cpp}` (a1465cb) so ton-index-postgres can share it; it
> predates v1.3, was never emulator-new, never deleted. Core direction (§0) unaffected.

- Actor tree (`ton-trace-emulator/src/main.cpp:107-112`): DbScanner (secondary rocksdb on the node's
  work DB) + RedisInsertManager + TraceEmulatorScheduler (new 8th ctor param `db_event_fifo_path`,
  `TraceScheduler.h:101-107`). Trace sources ×4: RedisListener (SUBSCRIBE channel, payload = b64 BOC
  of ext msg), OverlayListener, McBlockEmulator (finalized), ConfirmedBlockEmulator — ALL feed one
  `insert_trace_` sink: decl `TraceScheduler.h:35`, defined inline in the ctor `:109-111`, signature
  `void(Trace, td::Promise<td::Unit>, MeasurementPtr)` (`Measurement.h:81` — an EM1 decorator must
  thread it). In `TraceScheduler.cpp` only `make_signed_trace_processor` (`:407-412`) wraps that
  sink; `make_finalized_trace_processor` (`:414-429`) is DEAD CODE, zero callers since fea09a9, and
  McBlockEmulator gets the RAW sink (`:274`), so finalized traces never reach
  `InvalidatedTraceTracker::add_finalized_trace`. Decorate in the CTOR: it runs before `start_up()`
  (`:135`) creates listeners and before `:408` copies the std::function BY VALUE. **Still the single
  cheapest in-process seam.**
- `Trace` struct (`TraceEmulator.h:127-175`): tree of `TraceNode` (`:21-125`) `{ address,
  transaction_root: RAW UNPARSED CELL, children }`, which also carries node_id / mc_block_seqno /
  block_id / finality_state (`:22-30`); THREE maps — `emulated_accounts`,
  `interfaces`/`committed_interfaces`, `committed_accounts` (`:147`, `unordered_map<StdAddress,
  block::Account>`). FinalityState Emulated/Confirmed/Finalized — block sources RE-EMIT the same
  trace with higher finality (in-place upgrade). Limits 256 txs / depth 32
  (`EmulationContext.h:36-37`) → `tx_limit_exceeded` (`TraceEmulator.h:133`, truncated tree).
- Interface detection RUNS at emulation time (`TraceInterfaceDetector.cpp`) on exactly the trace's
  own accounts (emulated + committed) — BETTER account coverage than the per-seqno ParsedBlock the
  postgres shadow actor sees. BUT detector set = **6** (JettonWallet/JettonMaster/NftItem/
  NftCollection/GetGemsSale/GetGemsAuction) vs production's **13**
  (`index-engine/src/BlockInterfacesDetector.h:8-14`, FullDetector — now includes
  GetGemsNftFixPriceSaleV4) — **no DedustPoolDetector** (DeDust classification degrades), no
  GetGemsSaleV4, no Multisig/Vesting/Telemint. NB v1.3 SPLIT ton-index-postgres: interface detection
  + scheduler moved to `index-engine/`; `--mch-shadow` still at
  `ton-index-postgres/src/main.cpp:280,629-637`.
- Redis output (`TraceInserter.cpp`; bare `:N` refs below are that file): per-trace MULTI, trace key
  = b64(ext_in_msg_hash_norm) (`:56`) — per-node msgpack `RedisTraceNode` (`Serializer.hpp:210-218`,
  fork of schema structs `:147-218`), interface HSETs, per-account ZADDs, account-state Lua CAS
  (`:187-210`), PUBLISH. v1.3 command shape: node/interface/root fields batched into ONE multi-field
  HSET (`:249`); `has_existing_trace` short-circuit on re-emits (`:57`, `:88-90`, `:103`) with a
  `kMaxExistingTraceFields=1000` guard (`:35`); `RedisInsertManager::insert` uses
  `redis_.transaction(false, false)` (`:343-350`) → EM2 additions inside the MULTI serialize against
  other inserters. **`_aai` correction**: the C++ ZADDs go to the BARE address key `"<wc>:<hex>"`
  (`:146`); `_aai:<account>` keys are written ONLY by Python (`pendings.py:271`) and read by Go
  (`emulated/repository.go:89`) — EM2's C++ writer must ADD that ZADD, it inherits none. Channels:
  `new_trace` always (`:258`) + NEW `new_pending_trace` (`:13`, published `:256`, only when ≥1
  Emulated node remains) — EM2 must choose which it keys off; others `new_finalized_txs`/
  `new_confirmed_txs` `:215`/`:217`, `new_commited_txs` `:222`, `new_account_state` (Lua `:193`),
  `tr_in_msg:<b64>` SET+EXPIRE 600 `:252-253`.
- Python precedent (`indexer/events/pendings.py`): subscribes `new_trace`, HGETALL, deserializes to
  the same SQLAlchemy objects, classifies with the OLD full engine, writes back `HSET <key> actions
  gzip(msgpack)` + `_aai:<account>` ZADDs + publishes `referenced_accounts`. Wire contract consumed
  by `ton-index-go/index/emulated/models.go:530-600` (+ gzip sniff `:750`); numeric stringify rule =
  `Action.to_dict()` (`indexer/core/database.py:665-671`), exclusion set `{start_lt, end_lt,
  start_utime, end_utime, opcode, trace_start_lt, finality, trace_end_lt, trace_end_utime,
  mc_seqno_end, trace_mc_seqno_end}` — note `finality` IS excluded. A C++ writer must reproduce it
  byte-for-byte.

## 2. Delta: emulator Trace → what mch consumes

mch entry = `SchemaTraceLoader::schema_to_trace(trace_id, vector<schema::Transaction>)` (+ interface
lookup via `ParsedBlockLookupSource` two-tier). Gaps:

1. Emulator structs are a near-1:1 FORK of schema::* but not type-compatible; `mc_seqno` lives on
   the wrapper node, not the tx (assign during conversion).
2. **Convert from the RAW CELLS** (`TraceNode.transaction_root`), NOT from the Redis msgpack: the
   msgpack drops extra currencies (`value` flattened to uint64) → permanent
   `has_extra_currencies=false` (engine reads it, `BlockTree.cpp:420`). In-process conversion avoids
   the loss entirely. CAVEAT: `Message.extra_flags` exists in BOTH the emulator and schema structs
   but is int_msg_info's `extra_flags:VarUInteger16`, NOT extra currencies — do not misread it as
   fixing this (emulator msgpack `Message.value` is `optional<uint64_t>` grams-only,
   `Serializer.hpp:150`, parsed `:291`; `schema::Message.value` is `optional<CurrencyCollection>`,
   `IndexData.h:190`; flag derived `SchemaTraceLoader.cpp:80`). Candidate parse reuse:
   `ParseQuery::parse_message` / `process_transaction_descr` (PRIVATE members of ParseQuery,
   `tondb-scanner/src/DataParser.h:26,33` — needs a small visibility seam) or retarget emulator's
   own `Serializer.hpp::parse_tx` to schema structs.
3. Interfaces: `Trace::Detector::DetectedInterface` (variant of detector Results) →
   `schema::BlockchainInterfaceV2` adapter is mechanical — detector Results are field PREFIXES of
   the V2 structs; everything `ParsedBlockLookupSource::tier1` reads exists
   (`ParsedBlockLookupSource.cpp:46-131`; kind branches: jetton_wallet `:59`, nft_item `:70`,
   nft_sale `:85`, nft_auction `:106`, dedust_pool `:117`).
4. Lookup-kind coverage from emulator detectors: jetton_wallet ✓, nft_item ✓, nft_sale ✓ (no V4),
   nft_auction ✓; **dedust_pool ✗** (add detector to `TraceEmulator.h:135-137` — APPEND ONLY, Python
   reads variant indices 0-5 positionally per `Serializer.hpp:674` + `MSGPACK_DEFINE :676`, six
   alternatives, order unchanged at v1.3 — or serve via tier-2 hook); nominator_pool / jvault_assets
   = tier-2-only by design.
4b. **TIER-2 IS UNWIRED (survey 2026-07-28) — the real interface gap.** Python pre-parses message
   bodies (`address_selectors.py`: stonfi v1/v2, Tonco, ext-in wallet payloads + chained jvault
   data_boc requests) because it fetches interfaces EAGERLY per batch and must know keys up front
   (both paths: event_classifier.py:411-421 kvrocks/PG; pendings.py:184-220 — prod runs
   `--use-combined-repository`, confirmed from the live unit). C++ is demand-driven: the two-phase
   collect pass produces the exact key set, so **address_selectors must NOT be ported** — instead
   the tier-2 hook must be wired (today: `MchClassifierActor.cpp:67` and EM1 both leave it unset →
   every non-participant lookup returns null). Bites: stonfi v1 dex_in miss REJECTS the swap match;
   Tonco deposit miss silently falls back to TON (WRONG asset, not a reject); nominator_pool +
   jvault classify 0 actions in C++ today. The 165-fixture gates CANNOT see this: SchemaGate
   synthesizes tier-1 AND injects a fixture tier-2 from the Python-gathered interfaces dict
   (superset incl. selector-extracted accounts + jvault data_boc). Tier-2 wiring = pre-EM2
   blocker: batched kvrocks/DB round per collect pass; jvault needs a 3-round chained resolver;
   dedust_pool source = the shared `dedust_pools` map (interface_repository.py:293-305), separate
   from the missing emulator detector.
5. Only ordinary txs emulated (no tick_tock — harmless). trace_id := the redis trace key (b64
   ext_in_msg_hash_norm), matching Python's `trace_external_hash_norm`.
6. `tx_limit_exceeded` traces are TRUNCATED — classify-with-flag or skip; must be a stats bucket,
   never silent.

## 3. Phases

**EM1 — in-process shadow (log-only).** Decorate `insert_trace_` sink. New:
TraceNode→schema::Transaction converter (from raw cells, §2.2); DetectedInterface→V2 adapter (§2.3);
thin classifier actor over `classify_trace` (MchClassifierActor sibling, emulator-shaped inputs);
`--mch-artifact <mch_ir.json>` flag, startup-fatal on bad artifact (mirrors postgres
`--mch-shadow`); FailureCategory + lookup-tier stats logging (enum + LookupStats already exist from
Phase G); tx_limit bucket. Redis writes UNCHANGED. Optional but recommended: classify-only mode
(Redis fully optional) so a test run needs only the node DB — zero risk to prod pending data.
**Capture mode**: dump converted inputs (schema::Transaction vector + V2 interface map, msgpack) to
files → local `mch-emu-replay` feeds them through the classifier with no emulator/DB deps; captures
become a real-traffic regression corpus.

v1.3 constraints on the EM1 design:
- Shadow classification MUST be fire-and-forget (`send_closure` to a sibling actor, never inline in
  the sink): v1.3 added concurrency budgets kMaxFinalizedBlocksInFlight=2 /
  kMaxConfirmedBlocksInFlight=8 / kMaxSignedBlockFetchesInFlight=64 (`TraceScheduler.cpp:23-25`) and
  block completion gates on the promise chain downstream of `insert_trace_`.
- Observed finality mix depends on `--db-event-fifo`: WITHOUT it, `alarm()` polls
  `get_last_mc_seqno` every 0.3s and the signed/confirmed branch is skipped (Emulated + Finalized
  only, 2 emissions/trace); WITH it, Emulated + Confirmed + Finalized (3). Re-emit dedup and stats
  buckets must handle both.
- No DB reads on the sink path — event-driven catch-up is serialized by `db_catch_up_in_progress_`
  (`TraceScheduler.cpp:77-94`). Free telemetry hook: the `health:ton-trace-emulator` hash
  republished every 1s (`TraceScheduler.cpp:431-448`).

**EM2 — write-back (Python replacement).** END STATE (user, 2026-07-28): classification is an
INTEGRAL STAGE of trace production — emulate → classify → insert atomically; an emulated trace is
NEVER visible without its actions, at every finality emission. Concretely: classify runs between
emulation completion and TraceInserter; result → `HSET <key> actions gzip(msgpack)` + `_aai` ZADDs
+ `referenced_accounts` publish inside the SAME MULTI as the trace insert (re-computed per
re-emission — tx set may change with finality). No channel subscription anywhere in our path —
pubsub channels are for downstream consumers (ton-index-go) only. EM2 reuses EM1's components
(converter/adapter/classifier core/stats) unchanged; only the sequencing flips: shadow observer →
pipeline stage gating the write. Requires: Action→msgpack serializer reproducing Python's
stringify/exclusion rules exactly (contract: models.go:530-600) + `Action.accounts` (mch produces
accounts since Phase G step 2) + **tier-2 lookup wiring** (§1.4b — hard blocker: without it
stonfi/Tonco/dedust degrade or emit WRONG assets and nominator/jvault emit nothing; must be
designed off the sink path, inside the classifier actor, one batched fetch per collect pass)
+ **basic-actions fallback parity** — VERIFY whether ClassifyCore
emits Python's unmatched-trace fallback (ton_transfer/call_contract/contract_deploy per
`try_classify_basic_actions`); if not, port it — "always with actions" must not regress
unknown-protocol traces vs today's Python output. A/B option before flip: run both writers,
compare `actions` fields on live traffic — NB Python runs the OLD full-coverage engine vs mch's
ported subset → diffs MUST be bucketed ("protocol-not-in-mch" ≠ mismatch).

**EM3 — coverage + retirement.** DedustPoolDetector append (or tier-2), GetGems V4, then retire
Python `--emulated-traces` (user's op decision).

**EM4 — drop the runtime IR dependency (user goal, 2026-07-28).** The prod binary must not
need `mch_ir.json` at runtime: either extend mch-codegen to emit the matcher layer too
(pattern trees / captures / build_programs / registration_order as compiled tables — full
codegen), or at minimum embed the IR bytes in the binary at build time. `--mch-artifact`
then demotes to a dev/test override; the twin gates keep the file-driven path (dev mode) —
guard embedded-vs-repo drift with a gate (embedded hash == ir/mch_ir.json hash). Sequenced
after EM2 stabilizes; not an EM1 blocker.

## 4. Known risks / notes

- Emulator target had NEVER been built in our env before 2026-07-30 (first Linux build of the whole
  mch+abi stack just happened; expect more LP64/clang findings class-of b936074a). Windows: emulator
  configures but needs emulator_static + redis++_static compiled; `-rdynamic` link flag = MSVC wart.
- mch matcher set = ported subset (32 matchers); emulated-traffic stats must bucket unknown-protocol
  traces separately before anyone reads "mismatch" numbers.
- Design-first discipline applies (per-phase design doc → lead ack → code), same as W12: EM1 touches
  the emulator (upstream component) — keep the diff surgical, it merges against upstream v1.3 later.
