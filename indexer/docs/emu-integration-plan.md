# MCH classifier × ton-trace-emulator integration (EM program)

> Self-contained: survey facts + locked direction + phased plan. Fresh-session entry
> point for the emulator-integration work. Status: PLANNED, EM1 not started (as of
> 2026-07-30; first Linux build of ton-mch-engine just landed — LP64 fix b936074a).
> Companion state: docs/ton-abi-plan.md (module/bridge as-built), docs/mch-cpp-plan.md
> §1 (gate set), docs/mch-cpp-prod-design.md (shadow-actor Phase G design).

## 0. LOCKED DIRECTION (user, 2026-07-30)

**In-process**: classifier becomes PART of ton-trace-emulator — no Redis hop between
emulation and classification. That is one of the purposes of the C++ classifier
migration. Redis remains only as the OUTPUT contract (ton-index-go pending endpoints).
The out-of-process subscriber option (mirror of Python's `--emulated-traces`) was
evaluated and REJECTED as the end state — kept below only as background.

Real-world testing runs on the remote server (archive node + work DB live there;
emulator must run as root; `ulimit -n 99999` before launch; runs indefinitely →
every test run time-boxed / killable — systemd transient units
(`systemd-run -p LimitNOFILE=99999 -p RuntimeMaxSec=…`) fit all three constraints).

## 1. Emulator facts (survey 2026-07-30, verified at merge-v12 PRE-v1.3)

> ⚠ STALENESS (2026-07-27 v1.3 merge): upstream/v1.3 touched 10 ton-trace-emulator
> files incl. Serializer.hpp, TraceInserter.cpp, BlockEmulator.*, DbEventListener.*
> (new), OverlayListener.cpp, CMakeLists. The facts below (esp. file:line refs, the
> RedisTraceNode serialization shape, variant indices, and the insert_trace_ seam)
> MUST be re-verified against the merged tree before EM1 design. Core direction (§0)
> unaffected.

- Actor tree (`ton-trace-emulator/src/main.cpp:107`): DbScanner (secondary rocksdb on
  the node's work DB) + RedisInsertManager + TraceEmulatorScheduler. Trace sources ×4:
  RedisListener (SUBSCRIBE channel, payload = b64 BOC of ext msg), OverlayListener,
  McBlockEmulator (finalized), ConfirmedBlockEmulator — ALL feed one `insert_trace_`
  sink (`TraceScheduler.h:91-93`, per-finality wrappers :296-311). **That sink is the
  single cheapest in-process seam.**
- `Trace` struct (`TraceEmulator.h:127-176`): tree of `TraceNode{ address,
  transaction_root: RAW UNPARSED CELL, children }` + `emulated_accounts` +
  `interfaces`/`committed_interfaces`. FinalityState Emulated/Confirmed/Finalized —
  block sources RE-EMIT the same trace with higher finality (in-place upgrade).
  Limits: 256 txs / depth 32 → `tx_limit_exceeded` (truncated tree).
- Interface detection RUNS at emulation time (`TraceInterfaceDetector.cpp`) on exactly
  the trace's own accounts (emulated + committed) — BETTER account coverage than the
  per-seqno ParsedBlock the postgres shadow actor sees. BUT detector set = **6**
  (JettonWallet/JettonMaster/NftItem/NftCollection/GetGemsSale/GetGemsAuction) vs
  production's 12 (`ton-index-postgres/src/BlockInterfacesDetector.h:10-15`) —
  **no DedustPoolDetector** (DeDust classification degrades), no GetGemsSaleV4, no
  Multisig/Vesting/Telemint.
- Redis output (`TraceInserter.cpp`): per-trace MULTI — HSET per-node msgpack
  `RedisTraceNode` (fork of schema structs, `Serializer.hpp:147-218`), interface HSETs,
  `_aai`-style ZADDs per account, account-state Lua CAS keys, PUBLISH `new_trace`
  <trace_key>; trace key = b64(ext_in_msg_hash_norm).
- Python precedent (`indexer/events/pendings.py`): subscribes `new_trace`, HGETALL,
  deserializes to the same SQLAlchemy objects, classifies with the OLD full engine,
  writes back `HSET <key> actions gzip(msgpack)` + `_aai:<account>` ZADDs + publishes
  `referenced_accounts`. Wire contract consumed by
  `ton-index-go/index/emulated/models.go:530-600` (+ gzip sniff :749); numeric
  stringify rule = `indexer/core/database.py:93-118` (exact exclusion set — a C++
  writer must reproduce it byte-for-byte).

## 2. Delta: emulator Trace → what mch consumes

mch entry = `SchemaTraceLoader::schema_to_trace(trace_id, vector<schema::Transaction>)`
(+ interface lookup via `ParsedBlockLookupSource` two-tier). Gaps:

1. Emulator structs are a near-1:1 FORK of schema::* but not type-compatible;
   `mc_seqno` lives on the wrapper node, not the tx (assign during conversion).
2. **Convert from the RAW CELLS** (`TraceNode.transaction_root`), NOT from the Redis
   msgpack: the msgpack drops extra currencies (`value` flattened to uint64) →
   permanent `has_extra_currencies=false` (engine reads it, `BlockTree.cpp:420`).
   In-process conversion avoids the loss entirely. Candidate parse reuse:
   `ParseQuery::parse_message` / `process_transaction_descr` (PRIVATE members of
   ParseQuery, `tondb-scanner/src/DataParser.h:26,33` — needs a small visibility seam)
   or retarget emulator's own `Serializer.hpp::parse_tx` to schema structs.
3. Interfaces: `Trace::Detector::DetectedInterface` (variant of detector Results) →
   `schema::BlockchainInterfaceV2` adapter is mechanical — detector Results are field
   PREFIXES of the V2 structs; everything `ParsedBlockLookupSource::tier1` reads
   exists (`ParsedBlockLookupSource.cpp:58-128`).
4. Lookup-kind coverage from emulator detectors: jetton_wallet ✓, nft_item ✓,
   nft_sale ✓ (no V4), nft_auction ✓; **dedust_pool ✗** (add detector to
   `TraceEmulator.h:135-137` — APPEND ONLY, Python reads variant indices 0-5
   positionally per `Serializer.hpp:673-676` — or serve via tier-2 hook);
   nominator_pool / jvault_assets = tier-2-only by design.
5. Only ordinary txs emulated (no tick_tock — harmless). trace_id := the redis trace
   key (b64 ext_in_msg_hash_norm), matching Python's `trace_external_hash_norm`.
6. `tx_limit_exceeded` traces are TRUNCATED — classify-with-flag or skip; must be a
   stats bucket, never silent.

## 3. Phases

**EM1 — in-process shadow (log-only).** Decorate `insert_trace_` sink. New:
TraceNode→schema::Transaction converter (from raw cells, §2.2); DetectedInterface→V2
adapter (§2.3); thin classifier actor over `classify_trace` (MchClassifierActor
sibling, emulator-shaped inputs); `--mch-artifact <mch_ir.json>` flag, startup-fatal
on bad artifact (mirrors postgres `--mch-shadow`) — *EM4 replaced this with the
compiled-in matcher table: classification is default-ON, `--mch-disable` turns it
off*; FailureCategory + lookup-tier
stats logging (enum + LookupStats already exist from Phase G); tx_limit bucket.
Redis writes UNCHANGED. Optional but recommended: classify-only mode (Redis fully
optional) so a test run needs only the node DB — zero risk to prod pending data.
**Capture mode**: dump converted inputs (schema::Transaction vector + V2 interface
map, msgpack) to files → local `mch-emu-replay` feeds them through the classifier
with no emulator/DB deps; captures become a real-traffic regression corpus.

**EM2 — write-back (Python replacement).** Classification result → `HSET <key>
actions gzip(msgpack)` + `_aai` ZADDs + `referenced_accounts` publish inside the SAME
MULTI as the trace insert. Requires Action→msgpack serializer reproducing Python's
stringify/exclusion rules exactly (contract: models.go:530-600) + `Action.accounts`
(mch produces accounts since Phase G step 2). A/B option before flip: run both
writers, compare `actions` fields on live traffic — NB Python runs the OLD
full-coverage engine vs mch's ported subset → diffs MUST be bucketed
("protocol-not-in-mch" ≠ mismatch).

**EM3 — coverage + retirement.** DedustPoolDetector append (or tier-2), GetGems V4,
then retire Python `--emulated-traces` (user's op decision).

## 4. Known risks / notes

- Emulator target had NEVER been built in our env before 2026-07-30 (first Linux
  build of the whole mch+abi stack just happened; expect more LP64/clang findings
  class-of b936074a). Windows: emulator configures but needs emulator_static +
  redis++_static compiled; `-rdynamic` link flag = MSVC wart to patch.
- mch matcher set = ported subset (32 matchers); emulated-traffic stats must bucket
  unknown-protocol traces separately before anyone reads "mismatch" numbers.
- Design-first discipline applies (per-phase design doc → lead ack → code), same as
  W12: EM1 touches the emulator (upstream component) — keep the diff surgical, it
  merges against upstream v1.3 later.
