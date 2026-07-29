# EM2 (`680d1c4`) — change inventory for scope review

> Purpose: a feature-wise map of commit `680d1c4` ("EM2: tier-2 kvrocks lookups via suspension
> fixpoint + actions write-back in trace MULTI + A/B replay tooling + msgpack wire hardening +
> emulator perf-test flags"), 68 files, **+10466 / −433**, so the diff can be re-scoped rather than
> re-read. Written for a re-implementation with a narrower scope: every group below carries what
> breaks if it is dropped.
>
> Reviewed against the diff file-by-file on 2026-07-29. Design sources used for intent:
> `indexer/docs/em2-tier2-design.md`, `indexer/docs/em2-writeback-design.md`,
> `indexer/docs/emu-integration-plan.md` §1.4b + §3.
>
> **The one distinction that drives narrowing**: this commit is TWO features that share a file set.
> The *write-back* (actions into the Redis MULTI) is what makes "an emulated trace is never visible
> without its actions" true. *Tier-2 lookups* do not make the trace carry actions — they make the
> actions it carries CORRECT (without them nominator/jvault emit nothing, dedust degrades, and Tonco
> used to publish a wrong asset). Scope verdicts below say `core (write-back)` or `core (tier-2)`
> accordingly, because a minimal re-implementation can ship one without the other.

---

## 0. Orientation — the two commits underneath

**EM1 — `6c6f3f4`** (20 files, +1414/−115). Put the mch classifier *in-process* in
`ton-trace-emulator` as a log-only shadow: a decorated `insert_trace_` sink, a
`TraceNode → schema::Transaction` converter working from the raw cells, a
`DetectedInterface → schema::BlockchainInterfaceV2` adapter, `MchClassifierActor`'s emulator-shaped
sibling, and the `--mch-artifact` / `--mch-capture` / `--mch-classify-only` flags. Redis writes were
untouched; classification results went to counters and a 10 s stats line only.

**EM1.5 — `3444c22`** (15 files, +905/−208). Reworked the lifetime and integration story that EM1
got wrong: `Trace::cell_anchor` (the block BOC root cells) so classification could outlive the block
emulator, a request/response classifier gating the insert instead of a BOC snapshot, the `EmuGate`
admission valve + deadline + sticky fail-open watchdog, and `--mch-classify-nonblocking`. This is the
commit that made the insert *wait* for classification — EM2 is what finally puts something in the
insert.

---

## 1. Size ledger

Doc + generated artifacts stripped out, the code diff is ~7 900 lines added. Where it went:

| # | Group | Added | Removed | Verdict |
|---|---|---|---|---|
| G1 | Trace-level tier-2 fixpoint driver (`ClassifyDriver`) | 349 | 0 | core (tier-2) |
| G2 | Suspension loop in `EmuClassifierActor` | 738 | 119 | core (tier-2) |
| G3 | kvrocks tier-2 backend (`EmuLookupActor` + `KvrocksDecode` + exports) | 801 | 0 (+130/−130 move) | core (tier-2) |
| G4 | Action → msgpack writer + payload type | 485 | 0 | core (write-back) |
| G5 | Write-back into the Redis MULTI (seam, finality guard, shadow writer) | ~290 | ~20 | core (write-back) |
| G6 | `unknown` row + `Trace` aggregates | ~110 | ~3 | core (write-back) |
| G7 | Bucket-B action fills (`ActionBuild`) | 269 | 7 | core (write-back), partly opportunistic |
| G8 | Wrong-value guard + determinism fixes (Tonco / DeDust / jvault) | ~70 | ~24 | core (tier-2) + opportunistic |
| G9 | `nft_item.content` through tier 1 | ~45 | ~8 | **opportunistic — a live production bug fix** |
| G10 | Admission work cap (`--mch-work-cap`) | ~70 | ~5 | opportunistic (hardening) |
| G11 | Fail-open watchdog fix (staleness off the `in_flight` branch) | ~22 | ~14 | core (tier-2), pre-existing defect |
| G12 | A/B replay tooling (`mch-ab-replay` + twins) | 2408 | 9 | enabling |
| G13 | Gate / test scaffolding (3 new hermetic gates + `--lookup-keys`) | ~2130 | ~25 | enabling |
| G14 | msgpack wire hardening (`MSGPACK_USE_DEFINE_MAP`) | 47 | 5 | **out-of-scope (own PR)** |
| G15 | Emulator perf-test flags (`--mch-stress-backfill`, `--mch-classify-deadline-ms`) | ~60 | ~5 | **out-of-scope (own PR)** |
| G16 | Design documents | 2120 | 0 | enabling (record of decisions) |

Two of the biggest line counts — G12 (2408) and G13 (2130) — are **verification infrastructure, not
product**. Together they are 57 % of the code diff. That is the first thing to look at when the diff
"feels too large".

---

## 2. Feature groups

### G1 — Trace-level tier-2 fixpoint driver

**Purpose.** Create the seam that does not exist today: a trace-wide collect pass that can gather
every lookup key tier 1 could not serve, fetch them in one batch, and re-classify.

**Files** (all new except the two rewirings):
- `ton-index-worker/ton-mch-engine/src/ClassifyDriver.h` (+211)
- `ton-index-worker/ton-mch-engine/src/ClassifyDriver.cpp` (+138)
- `ton-index-worker/ton-mch-engine/src/MchClassifierActor.cpp` (+12/−21) — postgres actor routed through the driver
- `ton-index-worker/ton-mch-engine/src/Classify.cpp` (part of +76/−23) — `run_dump` routed through the driver

**Why.** `em2-tier2-design.md` §4 / D1. The engine's own two-phase lookup fixpoint
(`BuildDriver.cpp:235-254`) is scoped to ONE `(matcher, block)` build invocation and calls
`LookupSource::fetch` synchronously, deep in the call stack. There is no trace-wide key set and no
place to intercept one — so `emu-integration-plan.md` §1.4b's "one batched kvrocks round per collect
pass" describes a thing that had to be built. §4.2 explains why it is a re-classify and not a resume:
`classify_trace` mutates its inputs (merges consumed blocks, reparents in post-process,
grows the spine in `unwind_deployments`), so a second call on the same `TraceContext` runs over a
half-eaten tree; every round rebuilds the context, and the expensive TLB parse is hoisted out.

The step-wise `ClassifyFixpoint` class exists because an *actor* cannot block inside a synchronous
fetch callback — it must hand control back to its mailbox between the pass and the response. The
synchronous `run_classify_fixpoint` / `classify_trace_fixpoint` wrappers use the same builders, so
the async path cannot drift from the offline one.

**What breaks if dropped.** No tier-2 at all: every lookup tier 1 cannot serve stays a miss, which is
the state `em2-tier2-design.md` §2 describes — `nominator_pool` and `jvault_assets` classify zero
actions, `dedust_pool` always misses (no emulator detector), and every counterparty account a spec
looks up is a miss. Note the driver itself is byte-neutral with no fetch configured: with no
`Tier2Fetch` it runs one pass and commits it, which is why `--classify` / `--actions` /
`--actions-prod` stayed byte-identical.

**Coupling.** Depended on by G2 (the actor drives it step-wise), G3 (`LookupWants` is the fetch
contract), G13's `--lookup-keys` and `Tier2SuspendTest`, and `mch-ab-replay` (G12).

---

### G2 — Suspension loop in `EmuClassifierActor`

**Purpose.** Make classification suspendable across a network round trip without stalling the
emulator, and answer every safety question that creates.

**Files**:
- `ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp` (+614/−116)
- `ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.h` (+124/−3)
- `ton-index-worker/ton-mch-engine/src/emu/EmuTypes.h` (tier-2 constants portion of +125/−14)

**Why.** `em2-tier2-design.md` §5 (D2). The mechanics: per-trace state in an id-keyed
`pending_` map (`PendingClassify`), one `send_closure` per round, resume in `on_lookup_batch` /
`on_lookup_failed`. Each of the four companions is load-bearing and each answers a specific way this
would otherwise fail:

- **Alarm-driven sweep (§5.5).** A suspended trace whose response never lands wedges the pipeline
  *permanently*: its insert promise never resolves, `in_progress_cnt_` never releases, and
  `finalized_blocks_inflight_` stays pinned against `kMaxFinalizedBlocksInFlight = 2`. This
  explicitly invalidates `em1-design.md` §16.3/Q7's "no per-request timer" decision, whose stated
  ground was "the classifier does no I/O on this path". A `static_assert` in `EmuTypes.h` pins the
  ordering `deadline + grace + sweep interval < kStallFailOpenUs`, so a swept trace always answers
  before the watchdog concludes the classifier is wedged.
- **Circuit breaker (§5.6).** 5 consecutive failures ⇒ open 30 s; while open every trace degrades to
  tier-2-less behaviour at *zero* latency, no socket touched.
- **Suspended cap `kMaxSuspended = 256` (§5.7).** A suspended trace retains its converted
  `schema::Transaction` vector (bodies as base64 BOC) plus the cell anchor for the whole round trip
  — a materially larger footprint than a queued request, so it cannot lean on the 1024 valve.
- **Per-`trace_id` serialization (§5.2).** Suspension breaks completion order for repeated emissions
  of one trace, which is harmless for stats and a correctness bug for the write-back.

Also here: `res.actions` clearing is satisfied *by construction* rather than by an explicit clear —
the fixpoint discards every superseded round's context, so exactly one pass ever reaches
`rows_from_pass` (`em2-writeback-design.md` §3's S14 constraint).

**What breaks if dropped.** Tier-2 becomes a synchronous blocking fetch on the classifier thread —
one actor serving ~28 traces/block, each potentially paying connect + 3 chained socket timeouts. That
is the head-of-line stall the whole design exists to avoid. Dropping only the *companions* (sweep /
breaker / cap) is worse than dropping the loop: it produces a pipeline that wedges permanently on a
dead store with no way out.

**Coupling.** Depends on G1 (drives `ClassifyFixpoint`), G3 (`EmuTier2Fetch`), G4 (serializes inside
the arena lifetime). Depended on by G13's `Tier2SuspendTest`, which required
`make_fixpoint` to be made `protected virtual` purely to substitute a scripted fixpoint.

---

### G3 — kvrocks tier-2 backend

**Purpose.** Turn a collect-pass key set into `Value`s out of kvrocks, over the Redis protocol.

**Files**:
- `ton-index-worker/ton-mch-engine/src/KvrocksDecode.cpp` (+448) / `.h` (+103) — **pure**: JSON payload → `Value`, plus `kvrocks_resolve_batch` doing the grouping and the jvault chain over an injected `PayloadFetch`
- `ton-index-worker/ton-mch-engine/src/emu/EmuLookupActor.cpp` (+134) / `.h` (+101) — transport only, one blocking `MGET` per round
- `ton-index-worker/ton-kvrocks-state/src/KvrocksState.h` (+15) — six `build_payload` declarations exported
- `ton-index-worker/ton-kvrocks-state/src/KvrocksState.cpp` (+130/−130) — **visibility only**: the definitions moved out of the anonymous namespace, bodies unchanged (verified: the diff is a symmetric move)
- `ton-index-worker/ton-trace-emulator/src/main.cpp` — the `--mch-kvrocks*` flag surface + startup validation (part of +178/−8)

**Why.** `em2-tier2-design.md` §6 (D3/D4/D5). kvrocks only, lead-confirmed: production runs it, the
C++ client already exists with pool/timeout/Sentinel support, and the emulator already links
`redis++_static`. Three sub-decisions worth keeping:

- **The pure/transport split** is what makes the whole mapping gateable with no store — `KvrocksDecode`
  has no redis dependency and lives in `mch-classify`.
- **The connection defaults are a deadlock and must be overridden** (§5.3): `ConnectionPoolOptions`
  defaults to `size = 1` and `wait_timeout = 0`, and `wait_timeout == 0` means `_cv.wait()` with *no*
  timeout — a shared `Redis` across four actors would park three of them on a condvar forever. Plus a
  10 s `connect_timeout` default inside a non-preemptible actor body. Hence `tier2_kvrocks_config()`
  pinning connect/socket/wait to 150/150/50 ms and pool ≥ 2, one `Redis` per actor.
- **The jvault chain lives inside the resolver** (D5): the spec issues one `lookup jvault_assets(addr)`
  and there is no `data_boc` lookup kind, so the intermediate keys are invisible to the engine and can
  never appear in a classify-level round. Step 2 reimplements ~10 lines of
  `address_selectors.py:128-139` (`derive_accounts_from_stake_pool`) — the one function there that is
  lookup semantics rather than eager prediction (§12.4).

**What breaks if dropped.** Same as G1's answer: no tier-2 data. Also note the `build_payload` export
is what lets `KvrocksDecodeTest` generate its vectors from the *writer's own* functions rather than
from a hand-copied format spec, which is the drift guard on a production wire contract. The export is
recorded in §10 W3 as a deliberate lead ruling against CLAUDE.md's "never modify main pipeline source
for dev tools", on the ground that it is visibility-only with zero behaviour delta.

**Coupling.** Depends on G1 (`LookupWants`). Depended on by G2, G13 (`--kvrocks-decode-test`), and it
adds a `ton-kvrocks-state` link edge to both `mch-emu` and `ton-mch-engine`.

---

### G4 — Action → msgpack writer + payload type

**Purpose.** Produce the exact bytes `ton-index-go` decodes out of the pending trace hash's `actions`
field, and a payload type that carries them without dragging mch value types into the insert.

**Files**:
- `ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp` (+378) / `.h` (+40)
- `ton-index-worker/ton-mch-engine/src/emu/EmuActionPayload.h` (+67)

**Why.** `em2-writeback-design.md` §2 + §3 + §4.1. This is the group carrying **all the contract
risk**; the design's own wave plan says so (§9: "W1-W2 are the critical path and carry all the
contract risk"). What it encodes:

- **The two-level presence rule (§2.7)** — top-level unfilled fields are *omitted* (Python never
  assigns the attribute, so it is not a key in `to_dict()`); composite fields are *present as nil*
  (a composite is a literal dict whose arity is its fill's). Conflating them is the trap; the
  comparator is strict decoded-map equality with no nil≡absent normalisation precisely so the gate
  carrying the contract risk gets no tolerance rule that could mask a real omission.
- **The stringify rule (§2.2)** — every non-excluded int becomes a decimal string, *including inside
  composites*; the five top-level exclusions stay msgpack integers. Go's decoder is strict in both
  directions and a single field-level type error aborts the whole `Unmarshal`, dropping the trace.
- **`float_text()` — CPython `repr(float)`, ported.** Not a nicety: `std::to_chars` would emit
  `1037` vs Python's `1037.0` and `1.03e+11` vs `103000000000.0`. Found by the W2 parity gate on
  real corpus `nft_transfer_data.{price,nft_item_index}`, which contradicted the design's own
  earlier claim that no ported fill produces a Float.
- **The §2.6/Q1 divergence** — see the bug list, §3.1 below.
- **`EmuActionPayload` carries BYTES, deleting a use-after-free class.** EM1.5's result handed the
  cell anchor back next to the `Action` rows and relied on member declaration order, with a comment
  conceding that a caller who moved `actions` out "is back to a use-after-free". Serializing inside
  `classify` means the anchor's obligation ends when `classify` returns; `EmuClassifyResult::anchor`
  and `std::vector<Action>` are both deleted from the result.

**What breaks if dropped.** There is no write-back. This is the irreducible core of the stated end
goal.

**Coupling.** Depends on `ActionBuild` (G7 for the new fills, G6 for `create_unknown_action`).
Depended on by G2 (single call site `build_payload`), G5 (the insert), G12 (`mch-ab-replay` calls
`serialize_actions` directly), G13 (`--actions-msgpack` + `--actions-msgpack-test`).

---

### G5 — Write-back into the Redis MULTI

**Purpose.** Put the payload into the same transaction as the trace it describes, with a guard that
keeps the two describing the *same emission*.

**Files**:
- `ton-index-worker/ton-trace-emulator/src/TraceInserter.cpp` (+146/−3) — `append_actions()` + `ActionsOnlyInserter`
- `ton-index-worker/ton-trace-emulator/src/TraceInserter.h` (+53/−5) — widened `insert()`, `ActionWriteConfig`, `ActionsOnlyInsertManager`
- `ton-index-worker/ton-trace-emulator/src/TraceScheduler.h` (+25/−7) — the 4-arg→3-arg sink bridge
- `ton-index-worker/ton-mch-engine/src/emu/EmuShadow.cpp` (+105/−33) / `.h` (+15/−5) — payload on all outcome paths
- `ton-index-worker/ton-trace-emulator/src/main.cpp` — `--mch-actions-field` / `--mch-aai-prefix` / `--mch-actions-only` + validation

**Why.** `em2-writeback-design.md` §4. Three parts:

1. **Seam widening (§4.2, settles `em1-design.md` Q10).** `ITraceInsertManager::insert` takes the
   payload; `TraceScheduler`'s ctor builds a 4-arg lambda and either decorates it or wraps it with an
   empty payload — so the 3-arg sink the four trace sources copy **does not change type**. No
   listener and no block emulator learns the write-back exists. The payload is a parameter of the
   insert, not a property of `Trace` (which already took `cell_anchor` from us).
2. **`actions_finality` guard (§4.3a) — MANDATORY.** The node blobs have been individually guarded
   since `TraceInserter.cpp:87-95` (skip the subtree when the stored node is more advanced);
   `actions` had no equivalent, so a stale emulated emission landing after a finalized one would
   overwrite finalized actions silently while the node blobs in the *same MULTI* correctly refused
   the downgrade. Two independent races produce this and per-trace ordering closes only one: each
   emission gets its own `TraceInserter` actor and those contend for a connection pool whose waiters
   are woken by a bare `condition_variable` — no FIFO, no fairness. The decision is extracted as the
   pure `mch::actions_write_is_stale()` so it can be gated hermetically, and it **fails open** on an
   absent or unparseable stored value (a guard that failed closed would wedge the field permanently
   on the first corrupt value).
3. **`--mch-actions-only` shadow writer (§8 phase 2).** Writes only the two extra keys into an
   *existing* hash via an `EXISTS`-guarded Lua `HSET`, publishes nothing. The EXISTS guard is not
   decoration: if our emulator wins the race against production's, an orphan hash with actions and no
   nodes is what `ConvertHSet` reads as a malformed trace. Startup validation refuses
   `--mch-actions-only` with production field names.

Also here: `mch_classify_state` on the trace hash, so degradation is visible in Redis without reading
logs, and the feature-off path returns *before* the marker, the otel attribute and the guard's
`hget` — so a build with no `--mch-artifact` issues exactly the pre-EM2 command set.

**What breaks if dropped.** No write-back (as G4). Dropping only the **finality guard** leaves a
silent data-corruption path that no test would catch: the trace and its actions disagree about which
emission they describe. Dropping only `--mch-actions-only` costs the phase-2 A/B, which is a
verification loss, not a product loss.

**Coupling.** Depends on G4. `ActionWriteConfig` is shared with G12's phase-2 comparison.

---

### G6 — `unknown` row + `Trace` aggregates

**Purpose.** Make "every trace carries actions" true in the strong sense — a classification that
*succeeds* and matches nothing must ship something, not an empty array.

**Files**:
- `ton-index-worker/ton-mch-engine/src/ClassifyCore.cpp` (+14/−3) / `.h` (+21) — `unknown_trace` flag
- `ton-index-worker/ton-mch-engine/src/ActionBuild.cpp` / `.h` — `create_unknown_action()`
- `ton-index-worker/ton-mch-engine/src/TraceLoader.h` (+38) / `.cpp` (+22) — the five `Trace` aggregates + `fill_trace_aggregates()`
- `ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp` (+11) — the call

**Why.** `em2-writeback-design.md` §6 + §7. `ton-index-go` reads `Classified` as "the `actions` field
exists", so an empty array is reported to API consumers as a *successful* classification — losing the
`unknown` row that today tells a consumer "we looked and found nothing classifiable". The basic-actions
fallback already existed on the failure path (`ClassifyCore.cpp:333-367`), and the plan's "VERIFY
whether ClassifyCore emits Python's unmatched-trace fallback" is answered: it does. What was missing
was Python's `try_classify_unknown_trace` branch — a different gap than the plan anticipated.

This is also where a **latent production bug** surfaced — see §3.6 below.

**What breaks if dropped.** Zero-action traces ship an empty array and are reported as successfully
classified with no actions, which is indistinguishable from a real "nothing here" and different from
what Python emits today. The 165-fixture corpus has **50** such traces (227 → 277 rows), so this is
not a rare path.

**Coupling.** Depended on by G4 (serializes the row), G12 (`py_actions.py`'s `render_unknown_row`),
G13 (`test_unknown_action`).

---

### G7 — Bucket-B action fills

**Purpose.** Stop dropping rows for btypes whose matcher *runs*: the protocol is recognised, a block
is produced, and then `build_action` returns false and the whole row vanishes.

**Files**: `ton-index-worker/ton-mch-engine/src/ActionBuild.cpp` (+258/−6), `.h` (+11/−1) — adds
`change_dns_record_data` to `mch::Action`; `emu/EmuClassifierActor.cpp` extends `scrub_arena_refs`
for the new column.

**Why.** `em2-writeback-design.md` §2.4 bucket B. Nine fills ported: `renew_dns`,
`nominator_pool_withdraw_request` (a present payout flips the type to `stake_withdrawal`),
`tonstakers_withdraw`, `tonstakers_withdraw_request`, bare `dex_deposit_liquidity` (stonfi v2
provide), bare `dex_withdraw_liquidity` (dedust / stonfi v2 withdraw), `nft_purchase`, `change_dns`,
`delete_dns`. `dns_purchase` stays unported and is bucket **C** — no matcher in `ir/mch_ir.json`
produces it (the design records its own earlier listing of it under B as a doc error).

The `nft_purchase` reversal is worth reading: the old named-skip note said there was no corpus
coverage and so nothing to A/B-gate. That reasoning **inverts** under EM2 — the matcher runs and
Python emits the row, so omitting it does not avoid a phase-1 diff, it *manufactures* one in
`real-mismatch`, the one bucket that must read zero, on GetGems traffic that is plentiful live.

**Zero of these fire on the 165-fixture corpus**, so the A/B twin cannot gate them; they are gated
hermetically in `--actions-msgpack-test` instead, with field names taken from the IR's own
`out`/`switch` lists and column sets from the matching Python `_fill_*`.

**What breaks if dropped.** Those protocols are recognised and then silently dropped from the pending
payload — DNS renewals, stonfi v2 provide/withdraw, dedust withdraw, nominator/tonstakers
withdrawals, GetGems purchases. Since Python emits them today, dropping this group means the flip
*loses* action coverage relative to the incumbent.

**Scope note.** `change_dns` / `delete_dns` were explicitly *deferred-approved* earlier (they need
the new `change_dns_record_data` column, which drags in the `scrub_arena_refs` edit in a W4-owned
file) and then landed anyway in this commit. That pair is the most separable slice here.

**Coupling.** Depended on by G4/G13. `bytes_hex()` (lowercase, Python `bytes.hex()` semantics — *not*
`td::buffer_to_hex`, which is uppercase) is a real correctness detail inside this group.

---

### G8 — Wrong-value guard + determinism fixes

**Purpose.** Stop the one site in the engine that answers a lookup miss with a plausible *wrong*
value; make re-classification deterministic so a byte-identity gate cannot flake.

**Files**:
- `ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp` (+15/−2) — TON fallback → clean reject
- `ton-index-worker/ton-mch-engine/src/host/HostDedust.cpp` (+8/−2) — iterate `consumed`, not the set
- `ton-index-worker/ton-mch-engine/src/BuildRuntime.h` (+23) — `WrongValueGuard`
- `ton-index-worker/ton-mch-engine/src/BuildDriver.cpp` (+9/−5) / `.h` (+4/−1) — guard attached to the FINAL pass only
- `ton-index-worker/ton-mch-engine/src/ClassifyCore.cpp` / `.h` — per-matcher attribution + carry-out
- `indexer/indexer/events/mch/specs/jvault.mch` (+7/−2) and `ir/mch_ir.json` (+3/−11) — unconditional reject gate

**Why.**
- **Tonco (§8.2/§8.3).** `HostTonco.cpp:339-341` answered a `jetton_wallet` miss with
  `Value::make_asset_ton()` — publishing a missing counterparty wallet as a **TON deposit**. The
  design enumerates both consumer families and finds this is the *only* such site. The fix is one
  branch: `return rt_ok(Value::null())`, which `tonco_liquidity.mch:59-60`'s existing
  `reject when r == null` turns into a clean, **action-scoped** rejection — the correctly-classified
  jetton transfer next to it survives.
- **DeDust determinism (§4.3 Gap 1).** `dedust_swap_legs` built an `unordered_set<const Block*>` and
  then iterated *that set* to collect notification blocks before a `stable_sort` by `min_lt` —
  so insertion order into the sort was address-dependent, and `stable_sort` preserves it for ties.
  Two `DedustSwapNotification` blocks with equal `min_lt` would get different `in_leg`/`out_leg`
  between two passes. Improbable, not impossible — and "improbable" is exactly what makes a
  byte-identity gate flake intermittently. The fixpoint's re-classify is what makes it reachable.
- **jvault Q-C.** `jvault.mch`'s non-early branch was not reject-gated on `assets` yet still read
  `assets.asset` / `assets.jvault_asset`, so an unresolvable stake wallet would emit an asset-less
  action rather than none. The gate is now unconditional.

The counter placement is deliberate and documented: `wrong_value_guard` is *not* a `FailureCategory`,
because that field holds trace-terminal reasons only and names `clean_reject` among the values
excluded from it. The `BuildDriver` plumbing exists so the guard is attached to the **final** pass's
env only — the dry passes run the same code against a deliberately empty table, and counting them
would multiply one event by the round count.

**What breaks if dropped.** Tonco: a wrong asset goes into the pending API instead of a log line —
the single worst output the write-back could produce, and the reason §7.1's original "degrade to
fallback" proposal existed. DeDust: an intermittent, address-dependent gate flake with no obvious
cause. jvault: asset-less actions.

**Scope note.** All three are behaviour changes with **deliberate re-baseline** status — see §4.

**Coupling.** The guard needs `BuildEnv::guard` (G8's own plumbing) and reports through G2's stats.

---

### G9 — `nft_item.content` through tier 1

**Purpose.** Fix a live production action loss.

**Files**: `ParsedBlockLookupSource.cpp` (+29/−3), `emu/EmuInterfaces.cpp` (+7/−2),
`KvrocksDecode.cpp` (part), `abreplay/RedisReplay.cpp` (part), `SchemaGate.cpp` (comment update).

**Why / what it fixes.** See §3.3 below — this is a genuine bug, not a feature.

**Scope verdict: opportunistic**, and the design says so explicitly (`em2-tier2-design.md` §10
W-post: "Deliberately its own wave, landing after W4 … none of it is on the tier-2 critical path").
It rode along because the tier-2 work had the interface plumbing open.

**Coupling.** Independent of tier-2 and of the write-back; it is coupled only to the *files* the
other groups hold open. It is the single easiest group to split into its own PR.

---

### G10 — Admission work cap

**Purpose.** Bound the work one trace can cost once admitted.

**Files**: `emu/EmuTypes.h` (`kMaxClassifyNodes = 64`, `shed_oversize`), `emu/EmuShadow.cpp` (the
check in the decorator), `ton-trace-emulator/src/main.cpp` (`--mch-work-cap`).

**Why.** `em2-writeback-design.md` §5. `kClassifyDeadlineUs` is checked once, at dequeue, before any
work — once admitted nothing can shed a trace, and the measured tail is 13.3 ms against a ~100 µs
average (a 130× spread, i.e. cost tracks trace *size*). The emulator's own `txs_count_limit_ = 256`
does **not** cover this: `increase_tx_count` has exactly one caller, inside the emulation of a single
transaction, so the *committed* portion of a finalized trace is ungated. The value 64 is measured,
not guessed: over 5 833 captured emissions the ungated (purely finalized) population peaked at 34
nodes; 64 rejected 0 of 5 833. It stays a flag because that corpus under-samples exactly the traffic
carrying the tail (21 mixed traces, none at the ceiling).

The seam choice matters: the check is in the decorator right after `make_view` (which already walked
the tree, so the count is free) and *before* `in_flight` is incremented — so an oversized trace never
consumes a queue slot and never crosses a thread.

**What breaks if dropped.** Nothing observable today (the cap rejected nothing in 5 833 samples).
This is pure tail-latency insurance against a population that has not been sampled. It is genuinely
separable.

**Scope verdict: opportunistic (hardening).** Note Q3's decision — over-cap ⇒ **no** `actions` field
rather than basic-actions rows — is the one place §7's policy deliberately stops, with a stated
reason: a basic-rows payload is reported to Go as a *successful* classification silently missing every
protocol action, and "classified, wrongly" is a state consumers do not handle.

---

### G11 — Fail-open watchdog fix

**Purpose.** Let the stall watchdog see a wedged classifier that never reaches the admission cap.

**Files**: `emu/EmuShadow.cpp` — `admit()` restructured (~22/−14).

**Why.** `em2-tier2-design.md` §5.6 item 2. The staleness test lived *inside* the
`in_flight >= kMaxInFlightClassify` (1024) branch, but the block emulators self-throttle
(`kMaxFinalizedBlocksInFlight = 2`, `kMaxConfirmedBlocksInFlight = 8`, ~28 traces/block ≈ 280
block-sourced traces in flight). Nothing guarantees the population ever reaches 1024 — so a
classifier wedged on a store that stopped answering would stall the pipelines indefinitely while the
watchdog never fired. Two lines of logic; removes a class of "wedged but under the cap" states the
structure could not express.

**What breaks if dropped.** With tier-2 on and no breaker, an indefinite stall. With the breaker
present this is defence in depth. This is a **pre-existing EM1.5 defect** that tier-2 makes
reachable, not something tier-2 introduced.

**Scope verdict: core (tier-2).** Should be preserved in any re-implementation that wires tier-2.

---

### G12 — A/B replay tooling

**Purpose.** Compare Python's `actions` payload against ours over real traffic, offline, before any
write goes near production Redis.

**Files** (2408 added, 9 removed):
- `src/abreplay/RedisReplay.cpp` (+663) / `.h` (+84) / `main.cpp` (+491) — the new **`mch-ab-replay`** executable
- `twins/ab_replay_diff.py` (+320) — the bucketing comparator
- `twins/cmp_actions_msgpack.py` (+217) — the W2 write-back parity gate
- `twins/py_consumed.py` (+187) — instruments `TestInterfaceRepository`, cross-language demand containment
- `twins/mk_replay_dump.py` (+91), `twins/mk_hset_dump.py` (+64) — dry-run input generators
- `twins/py_actions_msgpack.py` (+62) — Python twin of `--actions-msgpack`
- `twins/py_actions.py` (+65/−9) — render seam split + `PORTED_BTYPES` widening + unknown-row support
- `twins/README.md` (+164)

**Why.** `em2-writeback-design.md` §8 phase 1 — a second emulator writing into prod Redis is not safe
as-is (it would duplicate `new_trace` / `new_finalized_txs` / `new_commited_txs` publishes into live
consumers), so the offline replay must come first. `mch-ab-replay` is a separate CMake target because
it is the only binary that needs the emulator's wire structs (`Serializer.hpp`) — the fixture harness
should not grow a dependency on `emulator_static`.

**Measured results this produced** (the reason the group earned its size):
- Converter in isolation reproduces `--actions-msgpack` **byte for byte**, 277/277 rows.
- With lookups served from the dump's own `AddressInterfaces` — *what EM2 actually sees* — the corpus
  yields **272 rows, 14 lost**: 6 jvault, 4 dedust `jetton_swap`, 4 `nft_transfer`. The first two are
  the tier-2 gap; the third is G9's tier-1 defect, which **no other gate could see**.
- `py_consumed.py --compare` measured collect-pass demand ⊇ Python-consumed with **gap = 0** over 185
  consumed keys. That is the actual evidence for the plan's "address_selectors must NOT be ported"
  claim; before this it was an assertion.

**What breaks if dropped.** The flip has no evidence behind it. `real-mismatch = 0` over live traffic
is the stated flip criterion, and there is no other way to produce that number. A narrower
re-implementation could defer the *phase-1 sweep tooling* but should not defer
`cmp_actions_msgpack.py` — that is the gate that proves the wire contract.

**Scope verdict: enabling.** Splittable into its own PR *after* the writer, provided the writer is
not shipped to production first.

**Coupling.** `ab_replay_diff.py` parses `src/ActionBuild.cpp` by regex to learn which action types
the fills produce (with a `len(btypes) < 15` guard) — a real, if pragmatic, coupling to production
source. `py_consumed.py` requires G13's `--lookup-keys`. `mch-ab-replay` requires G1's driver, G4's
serializer, and G6's `create_unknown_action`.

---

### G13 — Gate / test scaffolding

**Purpose.** Gate everything above with no store, no Redis, no artifact, no corpus.

**Files** (~2130 added):
- `src/EmuActionSerializeTest.cpp` (+817) / `.h` (+13) — `--actions-msgpack-test`, ctest `mch_actions_msgpack`
- `src/Tier2SuspendTest.cpp` (+480) / `.h` (+19) — `--tier2-suspend-test`, ctest `mch_tier2_suspend`
- `src/KvrocksDecodeTest.cpp` (+400) / `.h` (+17) — `--kvrocks-decode-test`, ctest `mch_kvrocks_decode`
- `src/LookupKeyGate.cpp` (+216) / `.h` (+54) — `--lookup-keys` (corpus-driven, **not** in ctest)
- `src/main.cpp` (+53) — five new CLI modes
- `src/Classify.cpp` / `.h` — `DumpMode` enum replacing the `bool actions_mode`, plus `--actions-msgpack`
- `CMakeLists.txt` (+61/−2) — three new ctest entries (8 → 11), the `mch-ab-replay` target, new link edges

**What each asserts** (concretely):
- **`EmuActionSerializeTest`** — 12 case functions, all assertions on the *decoded* map: fixed-key
  set and exact key count (12), the §2.3 omissions, the presence rule, the §2.6 int divergence plus
  its nested-key control, the full `VType` table, the counters, `float_repr_matches_python` over 13
  literal CPython vectors, `create_unknown_action` over a synthetic 3-tx trace, all nine bucket-B
  fills (`change_dns` × 4 schema branches), `view_finality`, and the seven `actions_write_is_stale`
  cases including the two fail-open ones.
- **`Tier2SuspendTest`** — a real `td::actor::Scheduler` + real `EmuClassifierActor` subclass against
  a fake store: feature-off issues no fetch, all-null commits a later pass, a resolved lookup changes
  the payload, **an all-ERROR fetch is byte-identical to an all-NULL fetch**, delayed answers match
  prompt ones, three same-trace emissions complete in send order, the suspended cap bounds outstanding
  fetches at 256, a never-answered response is still swept and answered, the breaker opens after 5
  failures and then serves without touching the store — plus "exactly one answer per emission",
  "`in_flight` returns to zero", "the watchdog never tripped".
- **`KvrocksDecodeTest`** — vectors **generated by `KvrocksState::build_payload`**, not written down:
  all six kinds, the >2^64 decimal-string int256, `destroyed:true` reads as absent, the
  lowercase→uppercase address-case round trip, `latest_account_states` correctly *not* subject to the
  destroyed rule, and the jvault chain in 3 fetches with its partial-result case.
- **`LookupKeyGate`** — asserts only its own invariants (REQ-ODD = 0, CASE = 0, no broken traces,
  `rounds_exhausted = 0`); MISSING/SURPLUS are reviewed output, deliberately not assertions, because
  the fixture map is what Python *predicted* it might need, not what its matchers read.

**Two production seams exist only for these gates**, and both are documented as such:
- `EmuClassifierActor::make_fixpoint` is `protected virtual` so the suspension gate can substitute a
  scripted fixpoint ("Production never overrides it"). `PendingClassify` moved into the public header
  to be its parameter type.
- `actions_write_is_stale` was extracted from `TraceInserter`'s body into a free inline function in
  `EmuActionPayload.h`.

**What breaks if dropped.** The wire contract, the suspension safety properties and the kvrocks
mapping all become unverified. Of the four, `EmuActionSerializeTest` is the one that cannot be
dropped from any re-implementation — it is the only thing standing between the writer and a wire-type
slip that silently drops whole traces.

**Scope verdict: enabling.**

---

### G14 — msgpack wire hardening

**Purpose.** Stop the Redis wire format from depending on where a file happens to sit in CMake.

**Files**: `ton-trace-emulator/CMakeLists.txt` (+19/−1), `ton-trace-task-emulator/CMakeLists.txt`
(+5/−1), `ton-trace-task-emulator/src/TonConnectProcessor.h` (+23/−3).

**Why.** `MSGPACK_USE_DEFINE_MAP` decides whether every `MSGPACK_DEFINE` struct in `Serializer.hpp`
is a **map keyed by field name** or a **positional array**. It was `PRIVATE` on the
`ton-trace-emulator` *executable*, where it held only by accident of file placement — `TraceInserter.cpp`
is the sole includer of `Serializer.hpp` and happens to sit under `add_executable` rather than in the
core library. Moving that one file into the library (the natural refactor, since it is a core insert
path) would have flipped the format of every blob **with no compile error and no gate failure**. It is
now `PUBLIC` on `ton-trace-emulator-core`, the same target that supplies the header's include path,
so the flag cannot be separated from what it governs. `TonConnectProcessor.h`'s three structs were
pinned to explicit `MSGPACK_DEFINE_MAP` (a no-op today under that flag) with the producer contract —
`ton-emulate-go` encodes with `UseArrayEncodedStructs(false)` — written into the source.

**What breaks if dropped.** Nothing today; the definition move is behaviour-neutral. It removes a
latent landmine.

**Scope verdict: out-of-scope — this is its own PR.** It was found *while* verifying the write-back
(the new `mch-ab-replay` needs the flag to read node blobs), which explains but does not require the
bundling. It touches `ton-trace-task-emulator`, a component EM2 has nothing to do with.

---

### G15 — Emulator perf-test flags

**Purpose.** Drive the emulator harder than live traffic does, so the classifier's tail can be
measured.

**Files**: `ton-trace-emulator/src/TraceScheduler.cpp` (+14/−3), `.h` (+25/−7 shared with G5),
`main.cpp` (`--mch-stress-backfill`, `--mch-classify-deadline-ms`).

**Why.** `--mch-stress-backfill N` starts N masterchain blocks behind the head so the skipped seqnos
are enqueued as one catch-up burst; `--mch-classify-deadline-ms` makes the 200 ms queue deadline
tunable (and `EmuGate::classify_deadline_us` replaces the compile-time constant on the read path).
Both are labelled TEST ONLY in their help text.

**What breaks if dropped.** Nothing in production. Losing them means the perf numbers §5 and §10.1
depend on cannot be reproduced without a rebuild.

**Scope verdict: out-of-scope — its own PR.** `--mch-stress-backfill` in particular changes
`TraceEmulatorScheduler`'s startup path (`got_last_mc_seqno`'s `last_known_seqno_ == 0` branch,
including a new anchor of `last_emulated_seqno_`), which is upstream emulator behaviour unrelated to
classification.

---

### G16 — Design documents

`indexer/docs/em2-tier2-design.md` (+944), `em2-writeback-design.md` (+1171), `em1-design.md` (+5,
the Q7 supersession note). 2120 lines, 20 % of the diff.

Keep. Every decision above is traceable to a numbered section, and both docs carry `file:line`
citations that were re-verified against the tree. The §12 / §11 "corrections to the existing docs"
sections in particular record ten places where `emu-integration-plan.md`'s and `em1-design.md`'s
assumptions turned out to be wrong — that is the part a re-implementation would otherwise rediscover
the hard way.

---

## 3. Production bugs found during EM2 — verified, with locations

The brief listed five. All five check out, but **they are not all the same kind of thing**, and two
more turned up that the brief did not name. Classified honestly:

### 3.1 Python's own `actions` payload is undecodable by Go for some action types — REAL, partly fixed

`convert_numerics_to_strings`'s exclusion set is **top-level only**
(`indexer/indexer/core/database.py:147-174`), so every integer *inside* a composite is stringified —
while nine Go composite fields are declared `*int32` / `*int64` / `*uint64`. A msgpack str into
`*int64` errors, a single field-level type error aborts the whole `msgpack.Unmarshal`, `ConvertHSet`
returns an error, and `FillFromRawData` **drops the entire trace** from the pending endpoints. There
is no per-field tolerance and no configuration escape (both decode sites are bare `msgpack.Unmarshal`;
no `,string` msgpack tag exists in the repo). The review pass reproduced the decode failure
empirically.

**Fix in this commit (Q1, user + lead: diverge — emit integers):** the four fields mch can produce —
`jvault_stake_data.period` and `layerzero_packet_data.{src_eid,dst_eid,nonce}` — ship as msgpack
integers. Location: `ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp`, the
`jvault_stake_ints` / `layerzero_packet_ints` sets wired through `ActionField::int_keys` into
`pack_fields`. Gated by `EmuActionSerializeTest.cpp::test_composite_int_divergence` (including
`nested_period_str`, which pins that the divergence sets are flat). Bucketed as
`wire-type-int-fields` in `twins/cmp_actions_msgpack.py`.

**NOT fixed:** the other five composites (multisig ×3, `layerzero_send_data`,
`layerzero_dvn_verify_data`, cocoon ×2) still drop whole traces through the Python writer. Recorded
as an open item in `em2-writeback-design.md` §10 — "Separate ticket, outside EM2's scope, but it
should exist."

**MUST PRESERVE** in any re-implementation that writes the payload. It is four lines of table data
and it is the difference between a trace appearing in the API and vanishing from it.

### 3.2 `referenced_accounts` is an unauthenticated remote-kill surface — REAL, NOT fixed here

`ton-trace-ttl-tracker/main.go:378-381` does `tokens := strings.Split(data, ";")` and indexes
`tokens[1]` with no length check. There is no `recover()` anywhere in that module and the only
deferred call on that goroutine is `rtt.wg.Done()`, so a payload without a `;` takes the **whole
ttl-tracker process** down. Anyone who can publish on that channel can kill the service.

**This commit does not fix it** — the fix lives in a Go module this commit does not touch. What
landed is defensive discipline on the new publisher: both halves of the payload are *built*, never
interpolated (`account` is `wc:HEX`, `trace_key_` is base64). Comments at
`emu/EmuClassifierActor.cpp` (`build_payload`) and `ton-trace-emulator/src/TraceInserter.cpp`
(`append_actions`).

The channel is load-bearing, not decoration: the subscriber merges the account into its per-trace
address set and uses exactly that set at expiry to `ZREM` the trace's members out of `_aai:<addr>`.
Stop publishing it and every `_aai` entry for a non-transaction account leaks forever.

**PRESERVE the discipline; the actual hardening remains unowned** and should be raised as a ticket
against `ton-trace-ttl-tracker`.

### 3.3 `nft_item.content` is Null on tier 1, and it drops whole actions — REAL, FIXED

`ParsedBlockLookupSource::tier1` hard-coded `f.emplace_back("content", Value::null())` with the
comment "never read by the nft_item consumers (dns reads collection_address only)". That comment was
wrong. `get_nft_data` **faults** on it —
`if (content == nullptr || content->t != VType::Dict) return rt_fault("nft content is not a dict")`
(`host/HostNft.cpp:230-233`), faithfully mirroring Python's `"uri" in nft.content` raising on a
non-dict — and a fault rejects the match, so the `nft_transfer` row is not produced at all.

**This was a live production loss, not a replay artifact**: `emu/EmuInterfaces.cpp`'s
`to_v2(NftItemDetectorR::Result)` also dropped `content`, so every *emulated* trace whose NFT the
emulator itself detected lost the row. No existing gate could see it: `SchemaGate.cpp:205-224`
deliberately declines to synthesize the three nft kinds into V2 and routes them to a fixture-backed
tier-2 fallback, so `--classify-prod` / `--actions-prod` never exercise tier-1 `nft_item` at all. It
was found by G12's replay adapter — the first path with no fixture underneath it — as **4 lost
`nft_transfer` rows on the 165-fixture corpus**.

**Fix, four touch points, all in this commit:**
- `src/ParsedBlockLookupSource.cpp` — `nft_content()` builds a flat `Str→Str` Dict
- `src/emu/EmuInterfaces.cpp` — `v.content = r.content`
- `src/KvrocksDecode.cpp` — `nft_content()` from the writer's raw-JSON object (tier 2 would NOT have
  closed it otherwise)
- `src/abreplay/RedisReplay.cpp` — carries `NftItemSchema.content`

The flat-vs-nested question resolved as *unobservable*: nothing in the IR or either serializer reads
`meta`/`name`/`type` off the nft dict, and every corpus item's content is already flat.

**MUST PRESERVE** regardless of scope. This is the only group in the commit that fixes rows currently
being lost in production with no feature attached.

### 3.4 `td::Gzip` emits zlib framing, not gzip — REAL TRAP, AVOIDED (nothing to fix)

`td::Gzip::init_encode` calls `deflateInit2(..., windowBits=15, ...)`, i.e. **zlib** framing
(`0x78 0x9c`), not gzip (`0x1f 0x8b`). `ton-index-go` sniffs the two-byte gzip magic and, on a miss,
passes the bytes through *uncompressed* to `msgpack.Unmarshal` — which would fail and drop the trace.
Its *decoder* is fine (`inflateInit2(..., MAX_WBITS + 32)` auto-detects both), which is exactly why
the asymmetry is easy to miss.

**No existing code was broken** — nothing in C++ gzipped for a Go consumer before this commit. The
response is decision Q2: **ship uncompressed msgpack**. Go's sniff passes it through, so both forms
work. The only code artifact is the comment at `src/emu/EmuActionPayload.h`:
`// Bare msgpack (§2.8 / Q2: uncompressed — td::Gzip emits zlib framing, which ton-index-go's magic
sniff would miss)`.

**PRESERVE the decision, not a patch.** A re-implementation that "optimises" by adding
`td::gzencode` reintroduces the bug. Real gzip framing (`deflateInit2` with `15 + 16`) is the correct
route if compression is ever wanted.

### 3.5 Uninitialized-field wire nondeterminism — REAL, but TOOL-INTERNAL

In `src/abreplay/RedisReplay.cpp`'s mirror structs, default-initialized fields went onto the wire as
whatever was on the stack, which made `--make-dump` non-reproducible across rebuilds. The comment is
explicit: *"Classification never read them, so every gate stayed green while the dump bytes moved."*

**This file is new in this commit**, so there is no production regression to preserve. Two sibling
findings in the same file are the same class and worth carrying as lessons: a float→int cast on
`nft_item_index` is UB at the ~1e76 values fragment usernames actually reach (saturates to
`LLONG_MIN`), and a default-constructed `block::StdAddress` carries `workchainInvalid`, which packs
as `"-2147483648:00…0"` and then throws out the whole interface blob on the way back in.

**Preserve only if the replay tooling is preserved.**

### 3.6 `SchemaTraceLoader` never filled `Trace`'s five aggregates — REAL, FIXED (not in the brief's list)

`mch::Trace::{start_lt, end_lt, start_utime, end_utime, mc_seqno_end}` were **zero on the production
path** — and that is the loader **both** classifier actors use (`EmuClassifierActor` and
`MchClassifierActor`). Latent while nothing read them; the moment W4's `unknown` row existed, every
production `unknown` row would have shipped zero lt/utime/mc_seqno into the write-back.
`--actions-schema` / `--actions-prod` caught it immediately (200 diff lines, all 50 unknown rows).

**Fix (lead's ruling: in the loader, not at the call site):** `fill_trace_aggregates()` in
`src/TraceLoader.{h,cpp}`, called from `src/SchemaTraceLoader.cpp`. The fixture loader keeps reading
the authoritative trace header, so two independent sources stay cross-checked by an existing gate —
`--actions` (header-sourced) vs `--actions-schema`/`--actions-prod` (derived) must stay
byte-identical, and the header's five values *are* exactly that min/max on all 165 fixtures.

**MUST PRESERVE if the `unknown` row is preserved.** Fixing it at the call site would have left the
zeros in place for the next reader.

### 3.7 The fail-open watchdog could not see a dead classifier under the cap — REAL, FIXED (not in the brief's list)

See G11. Pre-existing EM1.5 defect, ~2 lines of restructuring in `emu/EmuShadow.cpp`'s `admit()`.
**MUST PRESERVE if tier-2 is wired**, since tier-2 is what makes the wedge reachable.

### 3.8 `MSGPACK_USE_DEFINE_MAP` held by accident of file placement — LATENT, FIXED

See G14. Not a live bug; a build-configuration landmine where the natural refactor silently flips a
production wire format. **PRESERVE, but as its own PR.**

---

## 4. Deliberate behaviour changes (re-baselines), for the record

These are not bugs and not neutral — they change output relative to the previous binary and/or
relative to Python, on purpose, with sign-off:

| change | effect | status |
|---|---|---|
| `HostTonco` TON fallback → clean reject | a Tonco deposit whose router wallet does not resolve is **dropped** where Python publishes it as a TON deposit | approved (Q-A/Q-B); A/B bucket `tonco-first-asset-reject`; counted as `wrong_value_guard`; **fires even with tier-2 disabled** |
| `jvault.mch` unconditional `reject when assets == null` | `jvault_unstake_request`'s non-early branch loses rows with an unresolvable stake wallet instead of emitting asset-less ones | approved (Q-C) |
| `nft_purchase` fill added | GetGems purchases now emit rows | approved (W6 reversal) |
| `create_unknown_action` | 227 → **277** rows on the corpus (one per zero-action fixture, 50 of 165) | approved (W4) |
| `nft_item.content` plumbed | 4 `nft_transfer` rows recovered, replacing 4 synthetic `unknown` rows (total steady at 272 in the interface-limited replay) | approved (W-post) |
| `kMaxTier2Rounds` 4 → 6 | more headroom before silent last-pass-commit | lead call on W1 evidence (2 of 165 fixtures landed exactly on 4) |
| uncompressed `actions` blob | Redis memory delta vs Python's gzip level 1, unmeasured | Q2 |
| BOC framing | `te6ccg…` vs `te6cck…` — same cell, td writes no CRC32C — but the **bytes a consumer receives change at the flip** | permanent accepted divergence (root-hash verified 97/97) |

---

## 5. SUGGESTED NARROWING

If the goal is re-stated minimally as *"every emulated trace carries classified actions"*, the diff
splits cleanly into four PRs. The dependency order is real; the sizes are the code-diff numbers from
§1.

### PR 1 — Write-back (the actual end goal). ~1 700 lines.

G4 (serializer + payload) + G5 (seam, MULTI, finality guard) + G6 (`unknown` row + `Trace`
aggregates) + the `EmuActionSerializeTest` half of G13 + `cmp_actions_msgpack.py` +
`py_actions_msgpack.py` + the `py_actions.py` render-seam split.

This alone makes "never visible without actions" true. It needs **no tier-2, no driver, no
suspension, no kvrocks**. Explicitly *include* the finality guard (§4.3a) — it is 30 lines and its
absence is a silent data-corruption path — and the §2.6 int divergence (§3.1), which is four lines of
table data.

Defer from this PR: `--mch-actions-only` (phase-2 shadow writer) unless phase 2 is actually being run.

### PR 2 — Tier-2. ~1 900 lines.

G1 (driver) + G2 (suspension) + G3 (kvrocks) + G8's Tonco/DeDust/jvault fixes + G11 (watchdog) +
`Tier2SuspendTest` + `KvrocksDecodeTest`.

Sequencing note: G8's DeDust determinism fix **must** precede any byte-identity gate over the
fixpoint, or the gate flakes intermittently. Beyond that, the four suspension companions (sweep,
breaker, suspended cap, per-round deadline) are not optional extras — a suspension loop without them
wedges the pipeline permanently on a lost response, which is strictly worse than no tier-2 at all.

The most separable slice *inside* this PR is `--lookup-keys` + `py_consumed.py` (~400 lines): it
produced the "gap = 0" evidence for not porting `address_selectors`, and once that evidence exists
it does not need to be re-produced.

### PR 3 — Bug fixes that must ship regardless. ~50 lines.

G9 (`nft_item.content`, four touch points). This fixes rows currently being lost in production and
carries its own gate coverage the moment `content` is plumbed. It is coupled to nothing but the files
the other work holds open. **Ship it first if anything.**

G6's `fill_trace_aggregates` travels with PR 1 (it is only reachable via the `unknown` row).

### PR 4 — Everything that is not EM2. ~110 lines + tooling.

- **G14 msgpack hardening** — its own PR. Touches `ton-trace-task-emulator`, which EM2 has nothing to
  do with, and is behaviour-neutral.
- **G15 perf flags** — its own PR. `--mch-stress-backfill` changes upstream emulator startup
  behaviour.
- **G10 work cap** — defer. It rejected 0 of 5 833 real emissions; it is insurance against an
  unsampled population. Ship it when there is traffic evidence, and keep the flag when you do.
- **G7's `change_dns` / `delete_dns`** — these two were already deferred-approved once (they need the
  new `change_dns_record_data` column plus a `scrub_arena_refs` edit) and can be deferred again. The
  other seven bucket-B fills should stay with PR 1, since Python emits those rows today and omitting
  them makes the flip a coverage regression.
- **G12's phase-1 sweep tooling** (`mch-ab-replay`, `ab_replay_diff.py`, `mk_*_dump.py`, ~1 900
  lines) — separable as its own PR, but it must land *before* the flip, not after. It is the only
  source of the `real-mismatch = 0` number the flip criteria require, and it is what found §3.3.

### What NOT to narrow away, in one list

1. The `actions_finality` guard (§4.3a) — silent overwrite of finalized actions by stale emulated ones.
2. The §2.6 integer divergence — four table entries; without them a whole trace disappears from the API.
3. Uncompressed msgpack (do **not** reach for `td::gzencode`).
4. `nft_item.content` on all four touch points — a live action loss today.
5. `fill_trace_aggregates` if the `unknown` row ships.
6. The `admit()` staleness fix, if tier-2 is wired.
7. The DeDust `consumed`-not-`include` iteration, if the fixpoint re-classifies.
8. `EmuActionSerializeTest` — the wire contract has no other guard.

---

## Appendix — file → group map

| File | Group(s) |
|---|---|
| `indexer/docs/em1-design.md` | G16 |
| `indexer/docs/em2-tier2-design.md` | G16 |
| `indexer/docs/em2-writeback-design.md` | G16 |
| `indexer/indexer/events/mch/specs/jvault.mch` | G8 |
| `ton-index-worker/ton-kvrocks-state/src/KvrocksState.{h,cpp}` | G3 |
| `ton-index-worker/ton-mch-engine/CMakeLists.txt` | G3, G12, G13 |
| `ton-index-worker/ton-mch-engine/ir/mch_ir.json` | G8 |
| `ton-index-worker/ton-mch-engine/src/ActionBuild.{h,cpp}` | G6, G7 |
| `ton-index-worker/ton-mch-engine/src/BuildDriver.{h,cpp}` | G8 |
| `ton-index-worker/ton-mch-engine/src/BuildRuntime.h` | G8 |
| `ton-index-worker/ton-mch-engine/src/Classify.{h,cpp}` | G1, G6, G13 |
| `ton-index-worker/ton-mch-engine/src/ClassifyCore.{h,cpp}` | G6, G8 |
| `ton-index-worker/ton-mch-engine/src/ClassifyDriver.{h,cpp}` | G1 |
| `ton-index-worker/ton-mch-engine/src/EmuActionSerializeTest.{h,cpp}` | G13 |
| `ton-index-worker/ton-mch-engine/src/KvrocksDecode.{h,cpp}` | G3, G9 |
| `ton-index-worker/ton-mch-engine/src/KvrocksDecodeTest.{h,cpp}` | G13 |
| `ton-index-worker/ton-mch-engine/src/LookupKeyGate.{h,cpp}` | G13 |
| `ton-index-worker/ton-mch-engine/src/MchClassifierActor.cpp` | G1 |
| `ton-index-worker/ton-mch-engine/src/ParsedBlockLookupSource.cpp` | G9 |
| `ton-index-worker/ton-mch-engine/src/SchemaGate.cpp` | G9, G13 |
| `ton-index-worker/ton-mch-engine/src/SchemaTraceLoader.cpp` | G6 |
| `ton-index-worker/ton-mch-engine/src/Tier2SuspendTest.{h,cpp}` | G13 |
| `ton-index-worker/ton-mch-engine/src/TraceLoader.{h,cpp}` | G6 |
| `ton-index-worker/ton-mch-engine/src/abreplay/*` | G12 |
| `ton-index-worker/ton-mch-engine/src/emu/EmuActionPayload.h` | G4, G5 |
| `ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.{h,cpp}` | G4 |
| `ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.{h,cpp}` | G2, G4, G7 |
| `ton-index-worker/ton-mch-engine/src/emu/EmuInterfaces.cpp` | G9 |
| `ton-index-worker/ton-mch-engine/src/emu/EmuLookupActor.{h,cpp}` | G3 |
| `ton-index-worker/ton-mch-engine/src/emu/EmuShadow.{h,cpp}` | G5, G10, G11 |
| `ton-index-worker/ton-mch-engine/src/emu/EmuTypes.h` | G2, G4, G10 |
| `ton-index-worker/ton-mch-engine/src/host/HostDedust.cpp` | G8 |
| `ton-index-worker/ton-mch-engine/src/host/HostTonco.cpp` | G8 |
| `ton-index-worker/ton-mch-engine/src/main.cpp` | G13 |
| `ton-index-worker/ton-mch-engine/twins/*` | G12 |
| `ton-index-worker/ton-trace-emulator/CMakeLists.txt` | G14 |
| `ton-index-worker/ton-trace-emulator/src/TraceInserter.{h,cpp}` | G5 |
| `ton-index-worker/ton-trace-emulator/src/TraceScheduler.{h,cpp}` | G5, G15 |
| `ton-index-worker/ton-trace-emulator/src/main.cpp` | G3, G5, G10, G15 |
| `ton-index-worker/ton-trace-task-emulator/CMakeLists.txt` | G14 |
| `ton-index-worker/ton-trace-task-emulator/src/TonConnectProcessor.h` | G14 |
