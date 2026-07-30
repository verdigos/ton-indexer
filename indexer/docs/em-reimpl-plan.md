# EM re-implementation plan (W0–W4)

> Baseline: `merge-v13` @ `4b07ad4` (= `38d1e66` static parse seam + the two audit docs). The old EM
> work is archived on branch **`em2-archive`** (`680d1c4` EM2 ← `3444c22` EM1.5 ← `6c6f3f4` EM1) and is
> **reference only** — nothing is cherry-picked, everything is re-derived per the scope verdicts in
> `em2-change-inventory.md` and the class A/B/C/D split in `python-parity-audit.md`.
>
> Wave structure is fixed by the lead and is not re-ordered here.
>
> HEAD state confirmed by inspection: `ton-index-worker/ton-mch-engine/src/emu/` **does not exist**;
> `grep -rn mch ton-index-worker/ton-trace-emulator/src` = **zero hits**; `Trace` has no `cell_anchor`;
> `ParsedBlockLookupSource::Tier2Hook` is empty in production. W1–W3 are clean adds, no conflicts.
>
> **Both local build trees (`build/`, `build-rel/`) are stale** — they were configured against the
> reset-away EM tree and still list 11 ctest entries + `libmch-emu.a` / `mch-ab-replay`. Re-configure
> before trusting any gate run.

---

## 0. Deployment ordering rationale

The waves are ordered by *what can be deployed independently*, not by code size.

| wave | wire effect | deployable alone? | requires |
|---|---|---|---|
| **W0** Go decoder widening | reader only, strictly widening | **yes — deploy first, alone** | nothing |
| **W1** integration core | none (no `actions` written) | yes — observable via stats/logs | nothing (Go-independent) |
| **W2** write-back | writes `actions` + `_aai` + `mch_classify_state` | **no — W0 must be deployed** | W0 in production, W1 merged |
| **W3** celldb tier-2 | improves payload correctness | yes, after W2 | W1 (view plumbing) |
| **W4** harness split | none | yes, any time after W2 | W1/W2 file layout |

One operational task sits outside the waves: the **one-shot prod A/B sweep** (§8), run from an
`em2-archive` build after W2 and strictly before Python retirement. It blocks nothing, but it cannot
be run late.

**The Go reader must simultaneously tolerate old Python blobs and new C++ blobs.** Python stringifies
every int outside a five-entry top-level exclusion set; a natural-typed C++ writer does the opposite.
W0's `msgpack.Register` widening is *symmetric* (flexible int decoders **and** a flexible string
decoder), so both encodings decode identically on every field. That symmetry is what lets W2 drop the
Python stringify-mimicry entirely (audit §2.4 class D) instead of hand-maintaining a per-composite
exemption list.

W0 is not merely an enabler — it fixes production **today**: seven composite kinds
(`multisig_*` ×3, `layerzero_send_data`, `layerzero_dvn_verify_data`, `cocoon_*` ×2) currently drop
their whole trace out of the pending endpoints whenever Python writes them, and
`change_dns_record_data.flags` turns one malformed action into an HTTP **500 for the entire request**.

**Never deploy W2 to a cluster whose `ton-index-go` predates W0.** The failure mode is silent: one
field-level type error aborts the whole `msgpack.Unmarshal`, `ConvertHSet` errors, and
`crud/pending.go:181-185` logs and `continue`s — the trace simply vanishes from the API.

---

## 1. W0 — Go decoder widening

**Goal.** Make `ton-index-go` accept both msgpack encodings for every numeric and string field, so no
writer on either side has to reproduce the other's stringify rules.

> **STATUS: already in flight in the working tree.** `ton-index-go/index/emulated/flexdecode.go`
> (149 LOC) and `flexdecode_test.go` (~500 LOC) exist untracked, `models.go` is modified. This section
> is therefore written as a **specification + gap list to verify against**, not as an extraction map.

### 1a. File-level map

| file | change | LOC |
|---|---|---|
| `ton-index-go/index/emulated/flexdecode.go` *(new)* | `init()` registering `decodeFlexInt` for the 5 signed + `decodeFlexUint` for the 5 unsigned builtin int types, `decodeFlexString` for `string`; `parseFlexInt`/`parseFlexUint` accepting decimal **and** Python `str(float)` forms (`"1.0"`, `"1e+16"`), rejecting fractional values rather than rounding | ~150 |
| `models.go:285` | `actionChangeDnsRecordDetails.Flags *string` → `*int64` | 1 |
| `models.go:1058-1060` | delete the `strconv.ParseInt` + error return | −7 |
| `models.go:767` + `:929-932` | default `TraceExternalHashNorm` to the `traceKey` parameter when absent. Today that parameter is used **only** in an error string at `:773` | +3 |
| `models.go:869-871` | `ptrAccountAddress` — uppercase the hex after the `:`. 70 call sites funnel through it | +14 |
| `models.go:945`, `:1163`, `:1192`, **`:1315`** | the direct `models.AccountAddress(...)` conversions that bypass the helper | +4 |

Do **not** route address normalization through `ParseAccountAddressStruct` — three parse attempts per
address on a hot path.

### 1b. Verified facts that change the spec

- **Field census.** 189 declared `*string` (187 live), 46 numeric pointers (44 live), 24 non-pointer
  numerics (21 live). Only **7 distinct builtin numeric types** actually occur
  (`uint64, uint32, uint16, uint8, int64, int32, int8`); `int`, `uint`, `int16`, `float32`, `float64`
  never appear as fields. Registering all ten int kinds is harmless and future-proof — just note the
  audit's "ten int kinds" describes the registration, not the file.
- **`typeDecMap` is checked first** (`decode_value.go:49-56`), before the `CustomDecoder`/`Unmarshaler`
  checks and before any nil-aware wrapping. For `*T` where `T` is registered, `ptrValueDecoder`
  (`:126-140`) consumes `Nil` at `:129/:133` **before** dispatching — so the custom func never sees
  `Nil` on a pointer field. For a **non-pointer** registered field it does, and must handle it.
- **Registration must be in `init()`** — decoders are memoized into `typeDecMap` (`:53-54`) and struct
  field decoders are baked once at `structs.Fields()` time (`types.go:220-221`). Verified nothing in
  the repo decodes at package-init time.

### 1c. Two gaps the current implementation does not close

1. **`[]string` elements bypass the registered `string` decoder.** `_getDecoder` returns
   `decodeStringSliceValue` (`decode_value.go:105-107`) → `decode_slice.go:54` calls `d.DecodeString()`
   directly. Affected: `Action.TxHashes` :536, `.AncestorType` :559, `.Accounts` :561,
   `VestingAddWhitelist.AccountsAdded` :380, `JvaultClaim.ClaimedJettons` :407, and — the one that
   matters — **`JvaultClaim.ClaimedAmounts` :408**, a numeric-valued string list. `Action.Extra`
   `map[string]interface{}` :600 bypasses the same way.
   **DECIDED (lead):** `claimed_amounts` stays a decimal string; **no `[]string` registration**. The
   values are 256-bit-capable (§3b), so they had to be strings regardless — registering `[]string`
   would buy nothing and widen the Go surface for no consumer. W2's writer is bound to this.
2. **Named numeric types are not covered.** `msgpack.Register` is keyed on `reflect.Type`, so
   `AccountStatus` (:17), `AccStatusChange` (:41), `ComputeSkipReason` (:62) and
   `models.FinalityState` (`index/models/types.go:113`, underlying `uint8`) keep the stock decoder.
   All four arrive as msgpack ints from the emulator's own `Serializer.hpp` blobs and none is written
   by Python's action serializer, so **no action is needed** — record it so a future writer change does
   not walk into it.

### 1d. LOC

**~150 new + ~22 modified − 7 deleted** in product code; ~500 LOC of Go test. Net product ≈ **+165**.

### 1e. Verification

- `go build ./...` and `go vet ./index/emulated/` from `ton-index-go/`.
- `go test ./index/emulated/` — hermetic. Two hard constraints, both verified:
  - **must be an in-package test** (`package emulated`): `TraceNode.Transaction` is of unexported type
    `transaction`, so an external test package cannot build a node;
  - **do not round-trip through `msgpack.Marshal`**: `computePhaseVar` (:626) and `BouncePhaseVar`
    (:664) implement `DecodeMsgpack` expecting a 2-element array but have **no** `EncodeMsgpack`.
    Build blobs from `map[string]interface{}` — `decodeStructValue` accepts map-encoded structs.
  - `decompressGzip` (:750) passes non-gzip through untouched, so test blobs can be uncompressed.
- Minimum viable input: `{"root_node": K, K: <node blob>}`; the `actions` key is optional.
- **CI never runs `go test`** (`.github/workflows/tests.yml` runs pytest + a cmake build only).
  **DECIDED (lead): CI wiring is deferred until after W2** — it lands once the goldens and the wire
  contract exist, so there is something stable to gate. Until then this gate is **manual and must be
  run by hand on every touch of `index/emulated`**. Tracked as a W2 exit item (§3g).
- Regression assertion to include: for every currently-decoding blob shape, the decoded struct is
  **bit-identical** before and after. Widening must be a no-op on existing data.

### 1f. Risks / open questions

- Process-global registration. Acceptable: the only two `msgpack.Unmarshal` sites in the repo are
  `models.go:791` and `:826`.
- `decodeFlexString` renders a wire float with `strconv.FormatFloat(f,'g',-1,64)` → `"1037"`, where
  Python's `repr` gives `"1037.0"`. Not a decode error, but it *is* a visible API text change for any
  `*string` field a C++ writer chooses to emit as a float. W2's rule (§3b) avoids it by never emitting
  a float into a `*string` field.
- `crud/pending.go:241` compares `node.Key == traceKey` (normalized hash) while `:219` compares
  against `trace.ExternalHash` (raw `root_node`). The `:241` branch never fires when the two hashes
  differ. Pre-existing, out of scope, worth a ticket.

**Commit:** `ton-index-go: flexible msgpack int/string decoders + flags int, hash-norm default, address case`

---

## 2. W1 — integration core (EM1.5 product architecture minus harness)

**Goal.** Put the mch classifier in-process in `ton-trace-emulator` as a request/response actor whose
answer gates the Redis insert, with a cell-anchor lifetime that lets classification outlive the block
emulator — and nothing else.

### 2a. Extraction map — from archive

Source paths are `ton-index-worker/ton-mch-engine/` unless stated. LOC are at `3444c22` (EM1.5).

| new file | take from `em2-archive` | strip |
|---|---|---|
| `src/emu/EmuTypes.h` (132) | the EM1.5 form: `kEmuGlobalVersion=12`, `kMaxInFlightClassify=1024`, `kClassifyDeadlineUs=200'000`, `kStallFailOpenUs=5'000'000`, `emu_now_us()`, `EmuFinality`, `EmuCellAnchor`, `EmuTxRef`, `EmuTraceView`, `EmuClassifyOutcome`, `EmuClassifyResult`, `EmuGate`, `EmuShadowConfig` | every EM2 constant (`kMaxClassifyNodes`, `kMaxSuspended`, `kSweepIntervalSec`, `kSweepGraceUs`, `kBreakerFailures`, `kBreakerOpenUs`, the sweep `static_assert`); `EmuTier2Fetch`; `EmuGate::classify_deadline_us`; `EmuShadowConfig::{tier2_fetch,max_nodes}`; `EmuClassifyOutcome::shed_oversize`; includes of `ClassifyDriver.h`/`EmuActionPayload.h`. **Also strip EM1.5's `EmuShadowConfig::capture_dir`** (harness) |
| `src/emu/EmuTraceConvert.{h,cpp}` (23+27) | verbatim — `emu_to_schema_txs` over `ParseQuery::parse_transaction` | — |
| `src/emu/EmuInterfaces.{h,cpp}` (17+75) | the four `to_v2()` adapters | — (take EM2's 5-line `content` fix, §2c) |
| `src/emu/EmuClassifierActor.{h,cpp}` (69+355) | ctor, `classify(EmuTraceView, Promise<EmuClassifyResult>)`, `note_shed`, `start_up`, `alarm`, `remember_seen`, `EmuShadowStats`, file-statics `category_name`/`scrub_value`/`scrub_arena_refs`/`classify_txs` | the `write_capture` block inside `classify` (12 lines) + `EmuCapture.h` include + the `td::mkpath(capture_dir)` block in `start_up` |
| `src/emu/EmuShadow.{h,cpp}` (34+160) | `EmuTraceSink`, `make_view`, `decorate_sink`, `to_emu_finality`, `admit` | the **`nonblocking`** parameter and its whole branch (perf-measurement mode, audit H15/H26) |

**Do not create**: `EmuCapture.{h,cpp}` (210, → W4 `mch-record`), `EmuLookupActor.*`,
`EmuActionSerialize.*`, `EmuActionPayload.h`, `ClassifyDriver.*`, `KvrocksDecode.*`.

**Do not port any of** — the EM2 suspension surface on `EmuClassifierActor`, named for the checklist:
`~EmuClassifierActor`, `on_lookup_batch`, `on_lookup_failed`, `make_fixpoint`, `run_pass`, `suspend`,
`commit_pass`, `finish`, `answer`, `start_or_queue`, `release_trace`, `take_pending`, `sweep_pending`,
`arm_sweep`, `breaker_open`, `note_fetch_ok`, `note_fetch_failed`, `log_stats`; types
`EmuTier2Stats`, `PendingClassify`; members `tier2_`, `pending_`, `next_pending_id_`,
`inflight_traces_`, `queued_`, `breaker_failures_`, `breaker_until_us_`, `last_stats_us_`.

`EmuClassifyResult` member order for W1: `trace_id, outcome, **anchor**, **actions**, used_fallback,
unported_btypes, failure, failure_reason, failure_category, lookup_stats, queue_us, classify_us` —
anchor **before** actions, so reverse-destruction order frees the actions first. (W2 replaces both
members with a byte payload and the ordering constraint disappears with them.)

### 2b. Extraction map — emulator side

| file | change | LOC |
|---|---|---|
| `ton-trace-emulator/src/TraceEmulator.h` | `std::shared_ptr<const std::vector<td::Ref<vm::Cell>>> cell_anchor;` on `Trace`, after `tx_limit_exceeded` | 1 |
| `ton-trace-emulator/src/BlockEmulator.h` | the same member as `cell_anchor_` on **both** `McBlockEmulator` and `ConfirmedBlockEmulator` | 2 |
| `ton-trace-emulator/src/BlockEmulator.cpp` | build the anchor at the **top of both `start_up()`s** (Mc: one root cell per `mc_data_state_.shard_blocks_diff_` entry, reserved and filled *before* parsers spawn; Confirmed: `{block_data_state_.block_data->root_cell()}` as the first statement after `start_time_`), and `trace.cell_anchor = cell_anchor_;` as the first statement of both `trace_emulated` | ~30 |
| `ton-trace-emulator/src/TraceScheduler.h` | include `emu/EmuShadow.h`; members `mch::EmuShadowConfig mch_shadow_` + `ActorOwn<EmuClassifierActor> mch_classifier_`; ctor gains `EmuShadowConfig mch_shadow = {}` and — **in the ctor body, not a setter**, because `start_up` copies `insert_trace_` by value into the listeners — creates the actor and wraps `insert_trace_ = mch::decorate_sink(...)` | ~15 |
| `ton-trace-emulator/src/main.cpp` | **`--mch-artifact <path>` only** (master enable + IR load). Every other `--mch-*` flag stays unwritten | ~10 |
| `ton-mch-engine/CMakeLists.txt` | `add_library(mch-emu STATIC src/emu/{EmuTraceConvert,EmuInterfaces,EmuClassifierActor}.cpp)` + `PUBLIC src` + `cxx_std_20` + `PUBLIC mch-classify`; `add_library(mch-emu-bridge STATIC src/emu/EmuShadow.cpp)` → `PUBLIC mch-emu ton-trace-emulator-core`. `EmuShadow.cpp` is the **only** TU that includes `TraceEmulator.h` — that is what the bridge target exists for | ~20 |
| `ton-trace-emulator/CMakeLists.txt` | add `mch-emu-bridge` to the link line | 1 |

Listener-emulated traces leave `cell_anchor` null — that is correct and unchanged.

### 2c. Ride-along fixes (lead-mandated)

| # | file | change | LOC |
|---|---|---|---|
| R1 | `src/ParsedBlockLookupSource.cpp` | `Value nft_content(const optional<map<string,string>>&)` building a flat `Str→Str` Dict (`nullopt` → Null), replacing the hard-coded `f.emplace_back("content", Value::null())`. Take the `stats_.misses_by_kind[kind]++` in the same hunk | +32/−3 |
| R2 | `src/emu/EmuInterfaces.cpp` | `v.content = r.content;` in `to_v2(NftItemDetectorR::Result)`; `dns_entry` stays unset | +8/−2 |
| R3 | `src/TraceLoader.{h,cpp}` + `src/SchemaTraceLoader.cpp` | `Trace::{start_lt,end_lt,start_utime,end_utime,mc_seqno_end}`, `load_trace` sourcing them from the fixture header, new `fill_trace_aggregates()` (min/max over transactions), and the call at the end of `schema_to_trace`. **Inert until W2's unknown row reads them** — landing it in W1 is deliberate: fixing it at the call site instead would leave the zeros for the next reader | +53 |
| R4 | `src/host/HostTonco.cpp` | `first_asset = Value::make_asset_ton()` → `return rt_ok(Value::null())`, which `tonco_liquidity.mch:59-60`'s existing `reject when r == null` turns into a clean action-scoped rejection. **DECIDED (lead): land the reject WITHOUT the `WrongValueGuard` plumbing** — the archive's version needs `env.guard`, `BuildEnv::guard`, `BuildDriver` threading and `ClassifyResult::wrong_value_*` (~40 LOC of telemetry for a counter). Ship the correctness fix only; the miss rate does not need measuring | +6/−3 |
| R5 | `src/host/HostDedust.cpp` | in `dedust_swap_legs`, iterate the ordered `consumed` vector instead of the `unordered_set include` when collecting `DedustSwapNotification` hops — `stable_sort` preserves insertion order on `min_lt` ties, so set iteration made `in_leg`/`out_leg` address-dependent | +8/−2 |
| R6 | `src/ClassifyCore.cpp:203-207` | **no archive reference — write it new.** The region is byte-identical at `38d1e66`, `3444c22`, `680d1c4` and HEAD: `post_process_dedust_liquidity` does `std::sort(children_blocks.begin(), children_blocks.end())` on raw `Block*`, i.e. arena-address order, and the result reaches action output through `unwind_deployments` and the fallback rows. Replace the comparator with a deterministic key (min event-node `lt`, then `btype`, pointer only as a final tiebreak) and keep the `std::unique` de-dup by pointer identity | ~12 |

R6 is the one item in this wave with no reference implementation. It is *latent nondeterminism in the
actions* and W2 turns it into a wire-visible one, so it belongs here, before anything writes.

### 2d. LOC

| part | LOC |
|---|---|
| `src/emu/` (892 at EM1.5 minus capture) less the `nonblocking` branch | ~860 |
| cell anchor (3 files) | ~33 |
| `TraceScheduler` + `main.cpp` | ~25 |
| CMake (2 files) | ~21 |
| ride-alongs R1–R6 | ~120 |
| **total** | **~1 060** |

### 2e. Verification

- **Base gate set must stay green.** At HEAD: `ctest -R mch` = **8/8** in `ton-mch-engine`
  (`mch_expr_vectors`, `mch_expr_vectors_gen`, `mch_build_vectors`, `mch_shape_test`, `mch_dexrecords`,
  `mch_msg_vectors`, `mch_codegen_selftest`, `mch_abi_bridge`) plus `ton_abi_smoke` /
  `ton_abi_gen_check` = **2/2**. All ten are hermetic — inputs are committed JSON under
  `ton-mch-engine/ir/`; **none** touches `indexer/tests/traces/*.lz4` or `ir/mch_ir.json`.
  Re-configure the build tree first (both existing trees are stale, §0).
- **Twin gates: 165 fixtures → 227 action rows, diff 0.** Note the correct reading — 165 is the fixture
  count (`ls indexer/tests/traces/*.lz4 | wc -l` = 165), 227 is the Action-row count over them; it is
  **not** a 165-of-227 pass ratio. Run manually per `twins/README.md:9-19` from `indexer/`:
  `--parse-dump` vs `py_msg_dump.py`, `--match` vs `py_match_dump.py`, `--classify` vs
  `py_classify.py`, `--actions` vs `py_actions.py`. Not in ctest, not in CI, needs the live Python tree.
- **W1's expected diff on those twins: R1/R2 change it.** `nft_item.content` recovers `nft_transfer`
  rows that the fixture-backed gates could not previously see (`SchemaGate.cpp:205-224` declines to
  synthesize the three nft kinds into V2). R4 removes Tonco rows Python still publishes. Both are
  **deliberate re-baselines** — record the new counts in the commit, do not chase diff 0.
- Compile targets: `cmake --build <dir> --target mch-classify mch-emu mch-emu-bridge ton-trace-emulator ton-mch-engine`.
- New hermetic coverage: none required for W1 (no new decision logic beyond R6). R6 deserves one
  assertion — see W2's test TU, which is the first place a separate test target exists.

### 2f. Risks / open questions

- **`MSGPACK_USE_DEFINE_MAP` is still `PRIVATE` on the `ton-trace-emulator` executable at HEAD.** It
  holds only by accident of file placement: `TraceInserter.cpp` is the sole includer of
  `Serializer.hpp` and happens to sit under `add_executable`. Moving that file into
  `ton-trace-emulator-core` — the natural refactor — silently flips every Redis blob from map-keyed to
  positional-array framing, with **no compile error and no gate failure**. W1 does not move it and
  does not need the flag (the bridge TU includes `TraceEmulator.h`, not `Serializer.hpp`). Out of
  scope per the lead; leave the landmine documented, do not step on it.
- `--mch-artifact` stays a runtime IR load with a startup-fatal path. **DONE (EM4 "drop runtime IR"):
  the flag is deleted. `mch-codegen --matchers` compiles the artifact into `gen_matchers_ir()`,
  classification is ON by default, and `--mch-disable` is the ops escape hatch (null prep = today's
  no-flag behaviour). The startup-fatal property moved to `make_engine_prep()`, which now fails only on
  build bugs (parser-registry conflict, half-regenerated tables).**
- The gate is a *continuation*: `inner(...)` — the real insert — runs only inside the classify
  response promise. `~LambdaPromise` answers with an error on actor death, so `inner` fires exactly
  once on every path including shutdown. Preserve that property; it is the whole safety argument for a
  blocking gate.

**Commit:** `EM1: in-process mch classifier in ton-trace-emulator, cell anchor + gated insert`

---

## 3. W2 — actions write-back

**Goal.** Put the classified rows into the same Redis MULTI as the trace they describe, so an emulated
trace is never visible without its actions.

### 3a. Extraction map

| file | source | what to take / write |
|---|---|---|
| `src/emu/EmuActionPayload.h` *(new, ~67)* | `em2-archive` verbatim | `EmuActionPayload{actions_blob, aai, referenced_accounts, aai_score, action_count, state, finality}` + `actions_write_is_stale()`. **std-only includes are deliberate** — `TraceInserter.h` includes this without pulling `Value.h`/`ActionBuild.h` into the insert TU. Keep the uncompressed-blob comment verbatim (§3e) |
| `src/emu/EmuActionSerialize.{h,cpp}` *(new, ~320)* | `em2-archive` (378) minus class D | keep `pack_key`, `pack_text`, `omitted()`, `pack_fields`, `pack_value`, `pack_action`, `serialize_actions`, `kFixedKeys`, the 26-row `optional_fields()` table, the `ActionSerializeStats` counters |
| `ton-trace-emulator/src/TraceInserter.{h,cpp}` | `em2-archive` `append_actions()` | see §3c |
| `ton-trace-emulator/src/TraceScheduler.h` | `em2-archive` | widen the sink to 4-arg `insert(Trace, EmuActionPayload, Promise, Measurement)`; the ctor builds the 4-arg lambda and either decorates it or wraps it with an empty payload, so **the 3-arg sink the four trace sources copy does not change type** |
| `src/ActionBuild.{h,cpp}` | `em2-archive` | `change_dns_record_data` on `mch::Action` (1 line); the 10 bucket-B fills (171); `create_unknown_action` (27+11); `bytes_hex` (13) |
| `src/ClassifyCore.{h,cpp}` | `em2-archive` | `ClassifyResult::unknown_trace` + the `else if (result.actions.empty() && !ctx.trace.transactions.empty())` arm (15) |
| `src/EmuActionSerializeTest.{h,cpp}` *(new, ~700)* | `em2-archive` (817) minus obsolete cases | §3f |

**Strip under the natural-types rule** (Go now tolerates both directions):

| symbol | archive location | LOC |
|---|---|---|
| `float_text()` — CPython `repr(float)` reproduction | `EmuActionSerialize.cpp:75-104` | 30 |
| `jvault_stake_ints` / `layerzero_packet_ints` exemption sets | `:252-254` | 3 |
| `ActionField::self_int` / `::int_keys` members, the `int_keys` parameter on `pack_fields`/`pack_value`, and the `if (f.self_int)` branch in `pack_action` | `:129-140, :239-247, :347-351` | ~25 |
| the `mc_seqno_end` fixed key — Go has no such field, it was written "for parity" | `:331-332` | 2 |

Fixed keys go **12 → 11**. `bytes_hex()` **stays** — it is Python `bytes.hex()` semantics (lowercase,
unseparated), *not* a numeric-typing rule, and `td::buffer_to_hex` is uppercase; Go takes the field as
`*string`, so a case slip reaches API consumers silently.

### 3b. The natural-types rule, stated exactly

> Emit the natural msgpack type. A value-domain field that can exceed `int64` is a **decimal string**,
> because it does not fit an integer — a value-domain fact, not a Python quirk.

`Value::num` is `td::RefInt256` for **both** `VType::Int` and `VType::Amount`, so the decision is
per-field. Note `pack_int`'s `fits_bits(64)` check is **signed** — a `uint64` at or above 2⁶³ would nil
out under naive naturalization.

**Must stay decimal strings:**

| where | fields |
|---|---|
| top level | `amount`, `value` |
| `jetton_transfer_data` | `forward_amount`, `query_id` |
| `jetton_swap_data` | `min_out_amount`, `dex_incoming_transfer.amount`, `dex_outgoing_transfer.amount`, `peer_swaps[].amount_in`, `peer_swaps[].amount_out` |
| `nft_transfer_data` | `price`, `forward_amount`, `payout_amount`, `royalty_amount`, `query_id`, `nft_item_index` |
| `dex_deposit_liquidity_data` | `amount1`, `amount2`, `lp_tokens_minted`, `target_amount_1`, `target_amount_2`, `vault_excesses[].amount`, `nft_index` |
| `dex_withdraw_liquidity_data` | `amount1`, `amount2`, `lp_tokens_burnt` |
| `staking_data` | `tokens_minted`, `tokens_burnt` |
| `jvault_claim_data` | **`claimed_amounts`** — doubly required: 256-bit, *and* it is a Go `[]string` whose elements bypass the flexible decoder (§1c gap 1, resolved here by lead decision — there is no `[]string` registration to fall back on) |
| `vesting_send_message_data` | `query_id` |
| `evaa_withdraw_data` | `asset_id` (already a hex string via `av_hex`; do not renaturalize) |

**Safe to naturalize:** `opcode`, `flags` (uint8), `period`, `src_eid`/`dst_eid` (int32), `nonce`,
`tick_lower`/`tick_upper` (int24), `exit_code`, and the already-native `start_lt`/`end_lt`/
`start_utime`/`end_utime`/`finality`.

**Keep the two-level presence rule** (`omitted()` + "every composite key present, unfilled as nil").
The audit rates it deletable because Go tolerates both, but it is 37 LOC that encodes a real
distinction (a composite's arity is its fill's) and deleting it changes what consumers see for
partially-filled composites. Cheaper to keep than to re-derive; revisit only with a consumer complaint.

### 3c. The insert seam

`append_actions(trace_fields_to_set)` is called from `TraceInserter::start_up()` at the seam between
the `depth_limit_exceeded` field and the otel fields — i.e. **after both early returns**
(`existing_fields_count > 1000`, and `flattened_trace.empty()`), which is the correct coupling: a
skipped trace skips its actions too. Order inside:

1. `if (actions_.state == nullptr) return;` — feature off, **before** the otel attribute and before the
   `hget`, so a build with no `--mch-artifact` issues exactly the pre-W2 command set.
2. otel `ton.actions.count`.
3. `hget(trace_key_, actions_field + "_finality")` → `mch::actions_write_is_stale(stored, finality)`.
   **Mandatory.** The node blobs have been individually guarded since `TraceInserter.cpp:87-95`;
   `actions` had no equivalent, so a stale emulated emission landing after a finalized one would
   overwrite finalized actions while the node blobs in the *same MULTI* correctly refused the
   downgrade. Two independent races produce this and per-trace ordering closes only one — each emission
   gets its own `TraceInserter` actor and those contend for a pool whose waiters are woken by a bare
   `condition_variable` (no FIFO, no fairness). **It fails open** on an absent or unparseable stored
   value; a guard that failed closed would wedge the field permanently on the first corrupt value.
4. `mch_classify_state` HSET field — written *before* the empty-blob return, so "why there is no
   payload" is recorded even when there is none.
5. `if (actions_blob.empty()) return;` — the `actions` field stays **absent**, so Go reports
   `Classified:false`, which is exactly what a pending trace looks like before classification.
6. `actions` + `<actions>_finality` fields; `_aai:<account>` ZADDs, member `"<trace_key>:<action_id>"`;
   `publish("referenced_accounts", account + ";" + trace_key_)`.

**`referenced_accounts` discipline is load-bearing.** `ton-trace-ttl-tracker/main.go:378-381` splits on
`;` and indexes `tokens[1]` with **no length check**, and there is no `recover()` in that module — a
payload without a `;` takes the whole ttl-tracker process down. Both halves must be **built, never
interpolated**: the account is `wc:HEX`, `trace_key_` is base64. (The actual hardening belongs in a Go
module this series does not touch — raise it as its own ticket.) The channel is not decoration: the
subscriber uses the accounts it learns to `ZREM` the trace's members out of `_aai:<addr>` at expiry, so
an unpublished account leaks its index entry forever.

**Inline the key-name constants.** `ActionWriteConfig::{actions_field, aai_prefix}` were parameterised
only so the phase-2 A/B could rename them (audit H22); with no A/B they are `"actions"` / `"_aai:"`.
Do not write `--mch-actions-field`, `--mch-aai-prefix`, `--mch-actions-only`, `NullTraceInsertManager`
or `ActionsOnlyInsertManager`.

**`_aai` score:** Go fetches the score and never inspects it (`z.Score` unused; `GetActions` re-sorts
by `end_lt`). Write anything monotonic — `trace.start_lt` matches Python and costs nothing.

### 3d. Ancillary product deltas

- `EmuClassifyResult` drops `anchor` and `std::vector<Action>`, gains `EmuActionPayload payload` +
  `serialize_us`. Serializing **inside** `classify` means the anchor's obligation ends when `classify`
  returns — this deletes a whole use-after-free class that W1's declaration-order comment only mitigated.
- `scrub_arena_refs` (`EmuClassifierActor.cpp`) enumerates all 26 `Value` fields and must gain
  `&a.change_dns_record_data`. **Same set and order as `optional_fields()`** — two lists that must stay
  in sync; put a cross-reference comment at both sites.
- `serialize_actions`'s second parameter is `EmuTraceView` (a W1 type) — the one hard cross-wave
  coupling, satisfied by ordering.

### 3e. Compression: do nothing

`td::Gzip::init_encode` calls `deflateInit2(..., windowBits=15, ...)` — **zlib** framing (`0x78 0x9c`),
not gzip (`0x1f 0x8b`). `ton-index-go` sniffs the two-byte gzip magic and, on a miss, passes the bytes
through uncompressed to `msgpack.Unmarshal` — which would fail and drop the trace. Its *decoder* is
fine (`inflateInit2(..., MAX_WBITS + 32)` auto-detects both), which is exactly what makes the asymmetry
easy to miss. **Ship bare msgpack.** A later "optimisation" that reaches for `td::gzencode`
reintroduces the bug; real gzip framing (`deflateInit2` with `15 + 16`) is the correct route if
compression is ever wanted. Keep the comment in `EmuActionPayload.h` verbatim.

### 3f. LOC

| part | LOC |
|---|---|
| `EmuActionSerialize.{h,cpp}` (378 − ~60 class D) | ~320 |
| `EmuActionPayload.h` | ~67 |
| `TraceInserter.{h,cpp}` (`append_actions` 46 + config/ctor/plumbing) | ~100 |
| `TraceScheduler.h` sink widening | ~25 |
| `ActionBuild` 10 bucket-B fills + `bytes_hex` + `create_unknown_action` | ~220 |
| `ClassifyCore` unknown_trace | ~15 |
| `EmuClassifierActor` `build_payload` / `rows_from_pass` / stats | ~60 |
| **product subtotal** | **~810** |
| `EmuActionSerializeTest.{h,cpp}` (817 − obsolete cases) | ~700 |
| **total** | **~1 510** |

### 3g. Verification

- **New hermetic gate — its own TU, its own ctest entry**, per the harness rule:
  `src/EmuActionSerializeTest.{h,cpp}` compiled into the **harness exe only**, `add_test(mch_actions_msgpack ton-mch-engine --actions-msgpack-test)`.
  Assertions are on the **decoded map**, never on bytes — it is a Go-contract gate, not a Python-parity
  gate, and it is the only thing standing between the writer and a wire-type slip that silently drops
  whole traces.
  Cases to **keep**: `test_empty_actions`, `test_fixed_keys` (now 11), the omission/presence half of
  `test_null_and_stringify`, the presence half of `test_value_table`, the three counters,
  `test_multiple_actions`, `test_unknown_action` (end-to-end with `fill_trace_aggregates`,
  descending-input so min/max cannot pass by luck), `test_bucket_b_fills` (241 LOC, typing-agnostic),
  `test_empty_view_finality`, `test_view_finality`, `test_actions_write_guard` (7 cases including the
  two fail-open ones).
  Cases to **delete**: `test_composite_int_divergence` (48 — exists only for the exemption sets),
  `python_repr_cases_pass` (23 — dies with `float_text`), and the string-form assertions inside
  `test_null_and_stringify` / `test_value_table` / `test_counters`, **re-expressed** as: 256-bit fields
  are strings, bounded fields are ints.
  Add: one assertion per §3b string-list field (`claimed_amounts` especially), and one for R6
  determinism — classify the same fixture twice, assert byte-identical payloads.
- **G2 wire-contract round-trip:** a `go test` fixture in `index/emulated` consuming a blob the C++
  test emits. Strictly better than a C++ mirror of Go's expectations, and it is the natural home for
  the contract once Go is a place we change. Run manually for now.
- **W2 EXIT ITEM — wire CI.** Per the lead's decision (§1e), CI wiring waits for this wave, because
  W2 is what produces the two things worth gating: the frozen action goldens and the wire contract.
  Before W2 closes, add to `.github/workflows/tests.yml`: (a) a `go test ./index/emulated/` step,
  (b) a `ton-mch-engine` build plus `ctest -R mch` (the target is not built in CI today at all), and
  (c) the G1 golden diff. Until those land, every gate in this plan is manual — do not close W2
  without either the CI change or an explicit, recorded decision to keep gating by hand.
- Base gate set unchanged (10/10). Twins: `--actions` now emits **277** rows over the 165 fixtures
  (227 matched + 50 unknown, one per zero-action fixture) — a deliberate re-baseline, and
  `py_actions.py` supports it via a render-seam split.
- **Freeze the goldens in this wave.** Run `py_actions.py` green once and commit its output; there is
  **no committed golden for action rows anywhere in the tree today** (verified), and the generator dies
  with Python. This is the last cheap chance.

### 3h. Risks / open questions

- `--actions-schema` / `--actions-prod` (header-sourced vs derived aggregates) must stay
  byte-identical to `--actions` — that cross-check is what catches a `fill_trace_aggregates`
  regression. On the archive the fixture header's five values *are* exactly the derived min/max on all
  165 fixtures, with two knowingly-accepted Python divergences (`start_lt` = `min(lt)` where Python
  takes the root tx's lt; `mc_seqno_end` derived where Python leaves it `None`).
- The bucket-B fills fire on **zero** of the 165 fixtures — the twin cannot gate them, only the
  hermetic TU can. `dns_purchase` stays unported: it is bucket C, no matcher in `ir/mch_ir.json`
  produces it.
- `nft_purchase` must ship. Omitting it does not avoid a diff — Python emits the row, so omission
  *manufactures* one on GetGems traffic, which is plentiful live.

**Commit:** `EM2: write classified actions into the emulated trace MULTI with a finality guard`

---

## 4. W3 — celldb tier-2 (the wave with no reference code)

**Goal.** Serve the lookups tier 1 structurally cannot — accounts referenced only in message bodies —
by a **synchronous, in-process celldb read plus the existing smc-interface detectors**, and close the
biggest tier-1 miss bucket by widening the emulator's own detector set. No suspension, no fixpoint, no
circuit breaker, no kvrocks.

### 4a. Why this collapses the archive's 2 435 LOC

Everything in the EM2 tier-2 stack — `ClassifyDriver` (349), the suspension machinery in
`EmuClassifierActor` (322), `EmuLookupActor` (235), `Tier2SuspendTest` (499), the per-`trace_id` queue,
the alarm sweep, the breaker, the suspended cap — exists for exactly one reason: **`LookupSource::fetch`
is synchronous and buried in `BuildDriver.cpp:244`, and a network round trip cannot block an actor.**
A celldb read is local, synchronous and in-process, so it fits behind the existing seam and none of
that machinery is needed.

**The seam already exists and is empty in production:**

```cpp
// ParsedBlockLookupSource.h:40
using Tier2Hook = std::function<Value(const std::string &kind, const std::vector<Value> &args)>;
// ParsedBlockLookupSource.cpp:143-154
Value v = tier1(kind, args[0].str);
if (!v.is_null()) { stats_.tier1_hits++; return v; }
Value r = tier2_ ? tier2_(kind, args) : Value::null();   // ← install here
```

`BuildDriver::run_two_phase` (`BuildDriver.cpp:210-259`, max 8 rounds) already resolves
lookup-arg-of-lookup **within one build invocation**, and a synchronous source satisfies in-pass what
kvrocks needed a whole trace-level round for. `ClassifyDriver` is not needed at all.

### 4b. What exists in the tree (all verified)

| thing | where | note |
|---|---|---|
| `execute_smc_method(address, code, data, config, method_id, stack)` | `tondb-scanner/src/smc-interfaces/execute-smc.h:3-6` | **fully synchronous**, no promise, no actor. Needs only code+data+`ConfigInfo` |
| `lookup_account(AllShardStates&, StdAddress) → td::Result<schema::AccountState>` | `ton-smc-debug/src/DebugScanner.cpp:397-433` | **a free function, no actor** — the exact synchronous celldb fetch this wave needs, already written |
| `FetchAccountFromShardV2` | `tondb-scanner/src/smc-interfaces/FetchAccountFromShard.cpp:6-71` | the same code in an actor wrapper; its `start_up()` runs entirely synchronously — the actor buys nothing |
| `EmulationContext::get_account_state` | `ton-trace-emulator/src/EmulationContext.h:95-113` | a synchronous, memoizing arbitrary-address fetch already on the emulation hot path |
| **`DedustPoolDetector`** | `tondb-scanner/src/smc-interfaces/Tokens.h:152`, impl `Tokens.cpp:589-811` | **exists**. V1 (code-hash set + 4 get-methods + factory round-trip verification) and V2.0/2.1 (single `get_pool_data`, self-contained, no factory) |
| **`NominatorPoolContract`** | `smc-interfaces/NominatorPool.h:46`, impl `.cpp:126-174` | **exists**, and is a **pure TLB parse of the data cell — zero get-methods, zero shard access** |
| JVault detector | — | **does not exist anywhere**, and does not need to: jvault is a raw account-`data` chain |
| `InterfacesDetector<Detectors...>` | `smc-interfaces/InterfacesDetector.h:14-84` | variadic actor; a detector error is swallowed as "not this interface" (`:69-71`) |
| the emulator's detector set | `ton-trace-emulator/src/TraceEmulator.h:135-137` | **only 6**: JettonWallet, JettonMaster, NftItem, NftCollection, FixPriceSale, Auction. **No Dedust, no Nominator, no FixPriceSaleV4** |
| forward maps to `schema::*V2` | `index-engine/src/BlockInterfacesDetector.cpp:258-281` (Nominator), `:305-318` (Dedust) | copy-pasteable, already production code |

**Arbitrary out-of-trace fetch is already done today.** `DedustPoolDetector` fetches the DeDust factory
(`Tokens.cpp:728`), `JettonWalletDetectorR` the master (`:77`), `NftItemDetectorR` the collection
(`:253`), `MultisigOrder` the multisig (`Multisig.cpp:176`) — all through the shard-state dict. The
only constraint is that the address's shard prefix be covered by the loaded `shard_states_`, and those
are the **full shard set of one masterchain block** (`BlockEmulator.cpp:301-306`), so every mainchain
account is reachable.

### 4c. The failed-swap case, concretely

The class tier 1 cannot serve is *an address parsed out of a message body that never gets a
transaction*. Python has an entire prefetch module for it —
`indexer/indexer/events/blocks/utils/address_selectors.py:105-126` `extract_additional_addresses`, every
entry of which is a body-derived address.

C++ sites where the lookup argument comes from a parsed body:

| site | kind | provenance | failure today |
|---|---|---|---|
| `host/HostDedust.cpp:96` | `dedust_pool` | `parse_dedust_steps(cs)` walking the `SwapStep` chain of the jetton `forward_payload` / raw `swap_request` body | `rt_fault` at `:97-99` ⇒ **the whole `dedust_swap` action is rejected**. In a swap whose downstream hop never executes, the pools past the failure point exist *only* in the payload chain |
| `host/HostStonfi.cpp:860` | `jetton_wallet` | `extract_target_wallet(prev_body)` — literally the `if (!ok)` failed-swap branch | destination not overridden |
| `host/HostStonfi.cpp:674` | `jetton_wallet` | `pool_accounts(sv)` over cross-swap refs; the comment says *"only observed via the failed-swap fallback"* | asset unresolved |
| `host/HostStonfi.cpp:687`, `:402` | `jetton_wallet` | `parse_swap_v2(body).token_wallet1`, `stonfi_swap_body.jetton_wallet` | per-hop wallet unresolved |
| `host/HostTonco.cpp:55-58` | `jetton_wallet` | wallets parsed from `ToncoRouterV3PayTo` / `ToncoPoolV3SwapPayload` / `…FundAccountPayload` | was the **wrong-value** site; W1's R4 turns it into a clean reject, W3 turns it into a hit |
| `host/HostNft.cpp:212` | `nft_item` | transfer-body `nft_address` / auction `nft_addr` | counterparty NFT with no tx |

Not body-derived (envelope/tx-account, tier 1 normally serves them): `HostDedustDeposit.cpp:290,319,537`,
`HostNft.cpp:386,400`, and all five spec-level `lookup` nodes.

### 4d. Design

**Two independent halves. Ship them in that order — the first is three lines plus adapters.**

#### Half 1 — widen the emulator's detector set (closes the biggest miss bucket at tier 1)

`TraceEmulator.h:135-137` is a one-line variadic list. Add `DedustPoolDetector`,
`NominatorPoolContract` and `GetGemsNftFixPriceSaleV4` to it, and write the matching `to_v2()`
adapters in `EmuInterfaces.cpp` (forward maps already exist at `BlockInterfacesDetector.cpp:258-281`
and `:305-318`; `nft_sale`'s tier-1 branch already accepts either V1 or V4 —
`ParsedBlockLookupSource.cpp:85-105`).

CPU cost is near zero for non-matching accounts: both new detectors **gate on the code hash first**
(`Tokens.cpp:582-585`, `NominatorPool.h:17`) before running anything, and `NominatorPoolContract` never
executes a get-method at all. That is the argument for doing this despite
`InterfacesDetector`'s fan-out over every account in every trace.

`nominator_pool` is the cheapest kind in the system: the Value is `Obj{address}` and the whole
semantics is a code-hash predicate — Python does exactly this
(`interface_repository.py:868-870`, `NOMINATOR_POOL_CODE_HASH` at `:17`), and every consumer treats it
as set membership (`staking.py:336-338,363-365,418-420`).

#### Half 2 — the synchronous celldb tier-2 hook

**Plumbing.** `Trace` gains two members next to W1's `cell_anchor`, attached at the same two
`trace_emulated` sites: `AllShardStates shard_states` (= `std::vector<td::Ref<vm::Cell>>`, already held
as `McBlockEmulator::shard_states_`, `BlockEmulator.h:55`) and
`std::shared_ptr<block::ConfigInfo> config`. Both are cheap shared handles. `EmuTraceView` carries them
through; `make_view` fills them.

**The hook** — new TU `src/emu/EmuCelldbLookup.{h,cpp}` in `mch-emu` (`mch-classify` already links
`tondb-scanner`, so `smc-interfaces` and `execute-smc` are available with no new link edge):

```
Value celldb_tier2(kind, addr_str):
  1. block::StdAddress::parse(addr_str)                  // case-insensitive, matches tier 1
  2. memo lookup in the per-classify cache -> hit? return
  3. lookup_account(shard_states, addr) -> schema::AccountState   // synchronous celldb
  4. dispatch by kind -> detector body (below) -> schema::*V2
  5. insert the V2 into the per-classify side interface map
  6. return tier1(kind, addr_str)      // ← the shapes come from ONE code path
```

**Step 6 is the load-bearing design decision.** The hook never builds a `Value` itself: it produces a
`schema::BlockchainInterfaceV2`, files it into a side map, and re-runs `ParsedBlockLookupSource::tier1`.
Tier-1 and tier-2 shapes are then identical **by construction**, which is what the archive spent
`KvrocksDecode` (551) + `KvrocksDecodeTest` (417) proving by hand. It also means `nft_item.content`
(W1 R1) is served correctly on both tiers with no second implementation.

**Detector reuse.** The detectors are `td::actor::Actor`s, but 9 of 13 resolve their promise inside
`start_up()` and the 4 that fetch a second account do so through `FetchAccountFromShardV2`, whose
`start_up()` is itself fully synchronous. So: **add a static/free detect seam to `smc-interfaces`** —
`td::Result<X::Result> detect_X(addr, code, data, shard_states, config)` per detector, with the actor's
`start_up()` reduced to `promise.set_result(detect_X(...))`, and the second-account fetches swapping
`create_actor<FetchAccountFromShardV2>` for `lookup_account(...)` (identical work —
`FetchAccountFromShard.cpp:6-71` ≡ `DebugScanner.cpp:397-433`).

This is behaviour-neutral, shared with the postgres indexer, and follows the precedent already in the
base commit: `38d1e66` "tondb-scanner: static parse seam (`parse_transaction`, `parse_message`)". Write
it the same way. The alternative — re-implementing six detections in `mch-emu` against
`execute_smc_method` directly — duplicates ~200 LOC and guarantees drift.

**jvault needs no detector and no get-methods** (confirmed by the lead). The chain is three raw
account-`data` cell parses, ported from `indexer/indexer/events/blocks/jvault.py:36-69`:

1. `stake_wallet.data` → `load_cell_slice_special` → `load_address` ×2 = `(staking_pool, minter)`;
   `addr_none` pool ⇒ the whole record is null. `minter` → `jvault_asset = Asset(jetton)`.
2. `staking_pool.data` → `advance(1+32)` → `load_address` ×2 (admin, creator) → the **third** is
   `lock_wallet_address`. (~10 lines, the one function in `address_selectors.py:128-139` that is
   lookup semantics rather than eager prediction.)
3. `jetton_wallet(lock_wallet).jetton` → `Asset`.

Result `Obj{staking_pool, asset, jvault_asset}`. **The partial case is semantic, not an error**: pool
resolved but its state absent ⇒ `{staking_pool, asset:null, jvault_asset}`; missing lock wallet ⇒ the
whole record null. Celldb is *strictly better* than kvrocks here — `prepare_latest_account_state_row`
(`KvrocksState.cpp:1600-1606`) omits `data_boc` entirely when the account data exceeds
`max_data_depth`, and celldb never truncates.

**`dedust_pool` Value shape:** `Obj{assets: List[Obj{is_ton: Bool, address: Str|Null} × 2]}` — and
**only those two fields are ever read** (`HostDedust.cpp:104-127`). `reserve_*`, `fee`, `is_stable`,
`pool_type`, `dex` are never consumed by the classifier. `is_ton` is true ⟺ the asset slot is absent.

**Budget and memoization** (replacing the breaker/cap/sweep, ~40 LOC total):

- a per-classify memo `map<(kind, addr), Value>` — mandatory, not an optimisation: `BuildDriver`'s
  fixpoint re-runs `fetch` across rounds and every hop of a multi-pool swap asks for the same pools;
- a per-trace fetch budget (start at 16 distinct accounts) and a per-trace wall-clock budget
  (start at 20 ms), both counted in the hook, both degrading to `Value::null()` — i.e. exactly the
  tier-1-only behaviour we have today;
- a stats line per kind: fetches, hits, budget-exceeded. No breaker: there is no socket to break.

**`normalize_raw_address` is no longer needed for keying** (`block::StdAddress::parse` handles case and
the shard dict is keyed by raw 256 bits) but **remains needed for rendering** results
(`ParsedBlockLookupSource.cpp:17-23`), which is the A/B parity contract. Do not delete it.

### 4e. File-level map

| file | change | LOC |
|---|---|---|
| `tondb-scanner/src/smc-interfaces/{Tokens,NominatorPool,NftSaleV4}.{h,cpp}` | static detect seam per detector; actor `start_up()` becomes a one-liner over it; second-account fetch via `lookup_account` | ~90 (mostly moved) |
| `tondb-scanner/src/smc-interfaces/FetchAccountFromShard.{h,cpp}` | expose the synchronous body as a free `lookup_account(...)`; the actor keeps calling it | ~20 |
| `ton-trace-emulator/src/TraceEmulator.h` | `Trace::{shard_states, config}`; detector set gains Dedust + Nominator + FixPriceSaleV4 | ~5 |
| `ton-trace-emulator/src/BlockEmulator.cpp` | attach both at the two `trace_emulated` sites | ~4 |
| `src/emu/EmuTypes.h` | `EmuTraceView::{shard_states, config}` | ~4 |
| `src/emu/EmuShadow.cpp` | `make_view` fills them | ~4 |
| `src/emu/EmuInterfaces.{h,cpp}` | `to_v2` for `DedustPoolDetector`, `NominatorPoolContract`, `GetGemsNftFixPriceSaleV4` | ~60 |
| `src/emu/EmuCelldbLookup.{h,cpp}` *(new)* | the hook: parse, memo, `lookup_account`, kind dispatch, side-map insert, `tier1` re-entry, budget + stats | ~250 |
| `src/emu/EmuJvaultChain.{h,cpp}` *(new)* | the three-step data-cell chain | ~90 |
| `src/ParsedBlockLookupSource.{h,cpp}` | accept a side interface map the hook can write into, so step 6 sees it | ~20 |
| `src/emu/EmuClassifierActor.cpp` | install the hook when the view carries shard states | ~15 |
| `ton-trace-emulator/src/main.cpp` | one flag: `--mch-tier2` (default on when `--mch-artifact` is set), plus the budget overrides | ~10 |
| **product subtotal** | | **~570** |
| `src/EmuCelldbLookupTest.{h,cpp}` *(new, separate TU + ctest)* | §4f | ~250 |
| **total** | | **~820** |

### 4f. Verification

- **New hermetic gate, its own TU:** `add_test(mch_celldb_tier2 ton-mch-engine --celldb-tier2-test)`.
  What it can assert with **no live infra**:
  - **shape equality between tiers** — for each kind, build a `schema::*V2` by hand, run it through
    `tier1`, and run the same struct through the hook's step 5/6 path; assert identical `Value`. This is
    `KvrocksDecodeTest`'s job re-pointed, and it is cheap precisely because of the step-6 design;
  - the jvault chain over **committed data-cell BOC vectors** (three fixtures: full success, pool-state
    absent ⇒ partial record, lock wallet absent ⇒ null record);
  - the memo (same key twice ⇒ one fetch), the fetch budget (N+1th distinct account ⇒ null, and the
    trace still classifies), the time budget;
  - `nominator_pool` code-hash predicate: matching hash ⇒ `Obj{address}`, any other ⇒ null.
  A **fake shard-state source** is required for the fetch-path cases — inject the account lookup as a
  `std::function` so the test substitutes a map. Keep that injection point out of the product default
  (it is one `std::function` member, the same shape as `Tier2Hook` itself).
- Base gate set unchanged (10/10 + W2's `mch_actions_msgpack`).
- Twins: `--actions` row count rises (jvault and dedust rows that previously vanished). Expect the
  archive's measured shape — the interface-limited replay lost **14 rows: 6 jvault, 4 dedust
  `jetton_swap`, 4 `nft_transfer`**; W1's R1 recovers the last four, W3 the first ten. Re-baseline and
  record the number.
- **`--actions-prod` is structurally blind to sourcing** (`SchemaGate.cpp:265-269` installs a
  fixture-backed tier-2 over the Python-gathered superset). Do not read a green `--actions-prod` as
  evidence that tier-2 works; the hermetic gate above is the evidence.

### 4g. Risks / open questions

1. **Celldb GC under `dbs_secondary`.** The emulator opens `DbScanner` in secondary mode with a 0.05 s
   catch-up (`ton-trace-emulator/src/main.cpp:108`); shard-state roots are lazy `DynamicBagOfCellsDb`
   refs whose children are read on demand. Whether a held root still resolves after the primary's
   celldb GC has run is **not answerable from this tree**. Today the states are consumed within one
   mc-block cycle; a tier-2 fetch stays inside that window because classification is synchronous — but
   this must be confirmed on a live node before the flip. **Mitigation if it bites:** treat a failed
   cell load as a miss (null), never a fault.
2. **Latency.** No in-tree measurement of a cold celldb point read plus a TVM `run_get_method`. A
   `jetton_wallet` detection is 1 get-method + 1 account fetch + 1 more get-method (master
   verification) — materially more work than the MGET the archive's budget was written for. The fetch
   and time budgets exist for this; the numbers (16 / 20 ms) are guesses and must be re-set from a
   measurement in the first live run.
3. **`execute_smc_method` calls `args.set_now(td::Time::now())`** (`execute-smc.cpp:12`) — wall clock,
   not `gen_utime` — so get-methods that branch on time are not reproducible across runs.
   **DECIDED (lead): leave as-is.** Neither detector this wave adds time-branches —
   `NominatorPoolContract` runs no get-method at all, and `DedustPoolDetector`'s
   (`get_assets`/`is_stable`/`get_reserves`/`get_trade_fee`/`get_pool_data`) are all state reads.
   Time-branching lives in the GetGems auction (`end_time`) and DNS (`auction_end_time`) paths, which
   are pre-existing and unchanged. **Exclude those kinds from any byte-identity assertion**, and
   revisit only if a future detector time-branches — the fix then is to thread the emulation timestamp
   through the shared helper, a `tondb-scanner` signature change shared with the postgres indexer.
4. **`NFTItemDataV2.content` is a flat `map<string,string>`** while Python's content is a nested
   metadata dict, and `HostNft.cpp:230-263` uses it both as a flat `uri` lookup and as a whole-dict
   passthrough. The archive resolved this as *unobservable* (nothing in the IR or either serializer
   reads `meta`/`name`/`type`, and every corpus item is already flat) — re-verify rather than assume.
5. **Fan-out cost of the widened detector set** is bounded by code-hash gating in theory; measure it
   once on live traffic, since `InterfacesDetector` runs every detector against every account.

**Commit:** `EM3: synchronous celldb tier-2 lookups + dedust/nominator detectors in the emulator`

---

## 5. W4 — harness split and gates

**Goal.** Product libraries stop carrying fixture formats and A/B modes; the gates that matter get real
targets and real goldens.

### 5a. `mch-fixtures` (new test-only static lib)

Model to copy verbatim — `ton-mch-engine/CMakeLists.txt:194-204`, `ABI_BRIDGE_TEST_FIXTURES`: a named
source-list variable, sited next to a comment stating *why these files are test-only and must never
reach the prod lib*, spliced into exactly one target.

| move | from | note |
|---|---|---|
| `src/TraceLoader.cpp` (266) | `mch-classify` | the whole `.lz4`/msgpack fixture reader. **`grep` confirms lz4 and msgpack API use is 100 % inside this TU** — so `mch-classify`'s `lz4::lz4` link edge (`CMakeLists:191`) and the entire lz4 shim block (`:178-189`) move with it |
| `src/TraceLoader.h` | **stays** | it defines the core data model (`MsgContent`, `Message`, `Transaction`, `Trace`) that 9 files include. Only the `load_trace` **declaration** is fixture-only |
| **`load_trace_context`** (`src/BlockTree.cpp:463-472`, decl `BlockTree.h:217`) | **must be lifted** | **the hidden link break.** It sits in a core TU and calls `load_trace`; if `TraceLoader.cpp` leaves while `BlockTree.o` still references it, `ton-index-postgres` and `index-engine-core` **fail to link** (both pull `BlockTree.o` for `to_tree`/`init_block`). All six callers are harness |
| `FixtureLookupSource::fetch` + `upper_ascii` + `iface_obj` (`src/BuildRuntime.cpp:196-215, 232-396`) and the decl (`BuildRuntime.h:145-155`) | `mch-classify` | includes the 84-line jvault fixture chain at `:310-394` |
| `FixtureLookupSource::kinds()` (`BuildRuntime.cpp:219-230`) | **stays lib-side** | called from **production** code: `BuildDriver.cpp:134` (runnability gating) and `ParsedBlockLookupSource.cpp:43` (which literally returns it). Hoist the 7-name set to a free `lookup_kinds()` in the lib and have both classes call it |
| `vectors_generated.cpp`, `wheres_generated.cpp`, `builds_vectors_generated.cpp` | `MCH_GENERATED_SOURCES` | harness-only, verified: their `gen_*` symbols are referenced only from `VectorRunner.cpp:248`, `MatchRunner.cpp:113-121`, `BuildVectorRunner.cpp:222-242`. Split the variable into prod (`builds_ir_generated`, the testbed slot, `mch-msgs-tlb`) and test sets |
| `-DMCH_TESTBED_IR` table → `mch-fixtures`; **the knob survives** | `mch-classify` | **SUPERSEDED by EM4 "drop runtime IR".** The knob survives, but the whole sha-probe apparatus is gone: with no artifact loaded at run time there is nothing to probe against, so `MCH_TESTBED_IR` now just swaps the *input document* of the classify tables at configure time (`MCH_CLASSIFY_ARTIFACT`), under unchanged symbols. `select_gen_builds` / `select_gen_wheres` / `TestbedStubs.cpp` deleted; `prepare_classify` cross-checks the three tables' source shas instead |
| `install(TARGETS ton-mch-engine RUNTIME DESTINATION bin)` (`:227`) | — | drop it: a gate-only harness exe in the production bin dir |

### 5b. `mch-record` (new small tool)

The `EmuCapture` successor, as a **fixture writer, not a JSON debug dumper** — record real emulated
traces in the `.lz4` corpus format so they replay through the same loader as everything else. Nothing
in the product path: no `EmuShadowConfig::capture_dir`, no `mkpath` in the actor's `start_up`, no
`write_capture` on the classify path (audit H10–H13). W1 already omits all four sites, so this is a
clean new tool rather than a extraction.

Its value is G4 (audit §4): replay recorded traces through old and new binaries and diff the action
rows — buckets shrink from nine to two, *changed row* and *row count changed*. **The §8 sweep's
disagreement traces are its first corpus**, so keep `mch-record`'s output format loadable by the same
fixture reader those traces are archived in.

### 5c. Gates

**G1 — fixture actions gate (the workhorse).** `--actions` and `--actions-msgpack` over
`indexer/tests/traces`, diffed against **committed goldens** rather than a live Python twin. The
goldens are generated once from a green `py_actions.py` run — **that generation must happen in W2**
(§3g), because the generator imports the real Python classifier and dies with it. Thereafter they are
edited by review like any other expectation file. Target: harness exe + `mch-fixtures`.
Retires: `twins/`, `--surface`, `--parse-dump`, `--match`, `--classify`, `--build-smoke`.

**G2 — wire-contract self-test.** W2's `--actions-msgpack-test`, kept and extended. What remains to
assert after W0: `action_id` ↔ `_aai` member suffix, canonical addresses, std-base64 hashes,
`finality` in the emulator's 0/1/2 enum, and that the blob decodes at all. **A round-trip through the
real Go decoder** (a `go test` consuming a blob the C++ test emits) is strictly better than a C++
mirror of Go's expectations. Needs a CI decision (§1e).

**Not in scope:** the full base-tree parity cleanup — deleting `twins/`, `--surface`, `ExprEval`,
`IrLoader`, the dual conformance runners, and `MchClassifierActor` + its `--mch-shadow` wiring
(audit H7–H9, H27–H29, §2.5). That is a separate program and it must not land before G1's goldens exist.

### 5d. `TODO: remove (harness)` markers

Per audit §5.7, marker text `// TODO: remove (harness) — <destination, or "delete per
python-parity-audit §N">`. Only the H-sites that fall **inside files this series touches** are in
scope; the rest belong to the cleanup program.

In scope: H1 (`TraceLoader`), H2 (`FixtureLookupSource`), H3 (generated source split), H4 (testbed
knob), H5 (`GenBuilds.h` test-table decls), H30 (`DumpMode`'s `classify` arm), H31
(`TraceContextLoader`/`LookupSourceFactory`), H32 (`twin_view()`), H33 (the intermediate-dump flags),
H35 (the `install()`).

Not applicable — W1/W2/W3 simply never write them: H6 (`KvrocksDecode`), H10–H21 (capture,
classify-only, nonblocking, stress-backfill, deadline-ms, kvrocks flags, actions-field/aai-prefix/
actions-only and their validation), H22 (`ActionWriteConfig` key names — inlined per §3c), H23–H24
(`NullTraceInsertManager`, `ActionsOnlyInsertManager`), H26 (`decorate_sink`'s `nonblocking`),
H34 (`ClassifyDriver.h`'s gate-shaped invariant).

### 5e. LOC

| part | LOC |
|---|---|
| `mch-fixtures` CMake + the `load_trace_context` lift + the `kinds()` hoist | ~100 |
| `mch-record` | ~220 |
| G1 golden gate (runner + diff) | ~150 |
| G2 Go round-trip test | ~120 |
| markers | ~36 |
| **total** | **~630** (plus golden data files) |

### 5f. Verification

- `cmake --build` of **`ton-index-postgres` and `index-engine-core`** is the real gate for the
  `mch-fixtures` split — those are the two products that link `mch-classify`, and the
  `load_trace_context` break shows up nowhere else.
- `ctest -R mch` unchanged at 10/10 + the two new entries from W2/W3.
- G1's first run must reproduce the frozen goldens byte-for-byte; any diff is a regression, not a
  re-baseline.

**Commit:** `EM4: split mch-fixtures/mch-record out of the product libs, freeze action goldens`

---

## 6. Cross-wave must-preserve list

Carried from `em2-change-inventory.md` §5 "what NOT to narrow away", re-scoped to these waves.

1. **`actions_finality` guard** (W2 §3c step 3) — its absence is a silent overwrite of finalized
   actions by stale emulated ones. It **fails open**.
2. **Uncompressed msgpack** (W2 §3e) — do not reach for `td::gzencode`.
3. **`nft_item.content` on every touch point** (W1 R1/R2, W3 step 6 inherits it) — a live action loss
   today, ~45 LOC, no feature attached.
4. **`fill_trace_aggregates`** (W1 R3) — mandatory the moment the unknown row exists, and it belongs in
   the loader, not the call site.
5. **`referenced_accounts` built, never interpolated** (W2 §3c) — the subscriber is a remote-kill
   surface with no `recover()`.
6. **DeDust `consumed`-not-`include` iteration** (W1 R5) and **`ClassifyCore.cpp:204`'s comparator**
   (W1 R6) — both are action-visible nondeterminism once anything writes.
7. **The hermetic wire-contract gate** (W2 §3g) — the wire contract has no other guard.
8. **256-bit fields stay decimal strings** (W2 §3b) — including `claimed_amounts`, which is doubly
   required because Go's `[]string` path bypasses the flexible decoder.
9. **Do not deploy W2 against a pre-W0 `ton-index-go`** (§0).

---

## 7. Decisions (settled 2026-07-29, lead + user)

All eight open questions are resolved. Recorded here so the reasoning is not re-litigated mid-wave.

| # | question | decision |
|---|---|---|
| 1 | Go `[]string` element gap | **`claimed_amounts` stays a decimal string; no `[]string` registration.** 256-bit anyway, so it had to be a string regardless (§1c.1, §3b). The four named numeric types (§1c.2) need no action |
| 2 | CI wiring | **Deferred until after W2** — wire it once goldens + wire contract exist. **W2 exit item** (§1e, §3g). Gates are manual until then |
| 3 | `execute_smc_method` wall-clock `now` | **Leave as-is.** Neither W3 detector time-branches; exclude time-branching kinds from identity assertions; revisit only if a future detector needs it (§4g.3) |
| 4 | jvault mechanism | **Confirmed: three raw account-`data` parses, no detector, no get-methods** (§4d) |
| 5 | W1 R4 Tonco reject | **Confirmed: land the reject without the `WrongValueGuard` counter** (§2c R4) |
| 6 | `MCH_TESTBED_IR` | **Table moves to `mch-fixtures` in W4; the knob survives** — coffee/tgbtc coverage is testbed-only (§5a) |
| 7 | Python `--emulated-traces` retirement | **Strictly after the W2 golden freeze** (§3g) |
| 8 | `mch-ab-replay` / prod A/B sweep | **`mch-ab-replay` stays archive-only** — no A/B code carried to `merge-v13`. **The one-shot prod sweep is APPROVED** as an operational task, run from an `em2-archive` build before retirement — see §8 |

Two items stay **documented and unscheduled**, by decision, not oversight:

- **Stale build trees** (§0) — `build/` and `build-rel/` are configured against the reset-away EM tree
  (11 ctest entries, `libmch-emu.a`, `mch-ab-replay` present). Re-configure before trusting any gate
  run. The warning stays in the doc.
- **`MSGPACK_USE_DEFINE_MAP` is `PRIVATE` on the `ton-trace-emulator` executable** (§2f) — moving
  `TraceInserter.cpp` into `ton-trace-emulator-core`, the natural refactor, silently flips every Redis
  blob from map-keyed to positional-array framing with no compile error and no gate failure. Out of
  scope for this series; do not step on it, do not "clean it up" in passing.

---

## 8. Operational: the one-shot prod A/B sweep (pre-retirement, not a wave)

**Approved.** This is the single measurement that cannot be reconstructed after the fact, and it is
the reason `mch-ab-replay` is archived rather than deleted.

**What it is.** Compare Python's `actions` payload against the C++ writer's over real production
traffic, offline, and **archive the traces where the two engines disagree**. It is the only chance to
capture "Python classified this differently" at scale; once `--emulated-traces` is off, the reference
is gone forever.

**Constraints.**

- **Runs from an `em2-archive` build**, not from this series' tree. `mch-ab-replay` and the
  `twins/ab_replay_diff.py` bucketing comparator are not re-implemented in W0–W4, and **no A/B code is
  carried to `merge-v13`**. Build the archived branch, run it, keep the output.
- **Offline only.** A second emulator writing into prod Redis would duplicate `new_trace` /
  `new_finalized_txs` / `new_commited_txs` publishes into live consumers. The replay reads a prod HSET
  dump; nothing writes.
- **Ordering:** after W2 (so the writer under comparison is the one that ships), and strictly **before**
  Python retirement (decision 7). It blocks no wave.

**Known caveats when reading its output**, carried from the archive so they are not rediscovered:

- Expect a **non-zero `real-mismatch` bucket by construction** on the three approved re-baselines —
  the `HostTonco` clean reject (W1 R4), the jvault unconditional reject gate, and the `nft_purchase`
  rows Python emits. Classify those before treating any diff as a defect.
- The archive's own numbers, for orientation: the converter in isolation reproduced
  `--actions-msgpack` **byte for byte, 277/277 rows**; with lookups served from the dump's own
  interfaces — what the emulator actually sees — the corpus lost **14 rows (6 jvault, 4 dedust
  `jetton_swap`, 4 `nft_transfer`)**. W1 R1 recovers the last four and W3 the first ten, so a
  post-W3 sweep should show those closed.
- `ab_replay_diff.py` parses `src/ActionBuild.cpp` by regex to learn which action types the fills
  produce, with a `len(btypes) < 15` guard. That coupling breaks silently against a newer
  `ActionBuild.cpp` — run the comparator against the **archive's** source, or check the guard fires.

**Deliverable:** the disagreement traces, archived as fixtures. They seed G4 (audit §4) — replay
through old and new binaries, diff the action rows — which is what `mch-record` (§5b) exists to feed.
