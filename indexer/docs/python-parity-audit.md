# Python-mechanism-parity audit of the mch program

> Status: audit rev 2, 2026-07-29. Read-only sweep of `merge-v13` @ `680d1c4`. Scope = every feature
> or shape in the mch C++ classifier program whose *existence* or *form* is motivated by reproducing
> **Python's mechanisms** (its data sources, its intermediate pipeline stages, its request/response
> models, its stringify quirks, its staleness behaviours) rather than by **actions parity** — same
> trace in, same Action rows out.
>
> **Rev 2 incorporates three scope changes from the user:**
> 1. `ton-index-go` is **not** a frozen contract. Small Go changes are *preferred* whenever the
>    alternative is complex C++ written only to satisfy a reader quirk. New class **D — FIX IN GO
>    INSTEAD**.
> 2. **There are almost no frozen contracts anywhere.** Wire formats, Python counterparts and Redis
>    key shapes are all adjustable; the only near-goal is backward compatibility during a
>    transition. For every item the first question is *is it needed at all*, and the second is
>    *does a different component solve it cheaper* — not *delete or keep*.
> 3. **Harness / shadow / A-B machinery must not be woven into product code.** §5 inventories every
>    place it currently is, with enough precision for a mechanical cleanup sweep.
>
> LOC are `wc -l` on whole files unless a fraction is stated; every fraction says how it was
> estimated. Companion docs: `em1-design.md`, `em2-tier2-design.md`, `em2-writeback-design.md`,
> `emu-integration-plan.md`, `mch-cpp-plan.md` §1, `ton-mch-engine/twins/README.md`.

---

## 1. Summary

| class | meaning | LOC now | LOC after | net |
|---|---|---:|---:|---:|
| **A — DELETE** | pure verification scaffolding; no reason to exist once Python is not the reference | **4 925** | 0 | **−4 925** |
| **B — REDESIGN SIMPLER** | real function, shape forced by a now-dropped constraint | **4 458** | ~1 780 | **−2 678** |
| **D — FIX IN GO INSTEAD** | C++ contorting to match a reader quirk a small Go change eliminates | **126** | ~20 | **−106** (+~50 Go) |
| **C — KEEP** | actions parity, or a contract worth keeping because it is cheapest where it is | rest | rest | 0 |
| | **total mechanism-parity-driven** | **9 509** | ~1 800 | **−7 709** |

Denominator: 25 875 LOC of C++ in `ton-index-worker/ton-mch-engine` + 2 127 LOC of Python in
`twins/` = **28 002 LOC**. Mechanism parity accounts for **~34 %** of the program; **~28 %** of the
program disappears outright.

Excludes host-side edits in `ton-trace-emulator`, `ton-index-postgres` and `index-engine`, counted
inline where they belong (~470 LOC, of which ~320 is class A — see §5).

### Class A detail

| item | files | LOC |
|---|---|---:|
| Python twins + A/B tooling | `twins/*.py` (12 files) | 2 127 |
| A/B replay harness | `src/abreplay/{main.cpp,RedisReplay.cpp,RedisReplay.h}` | 1 238 |
| Collect-pass key-set gate | `src/LookupKeyGate.{cpp,h}` | 270 |
| Match-phase dump (`--match`, `--match-gen`) | `src/MatchRunner.{cpp,h}` | 225 |
| Message-parse dump (`--parse-dump`) | `src/ParseDump.{cpp,h}` | 222 |
| Converter-input capture (`--mch-capture`) | `src/emu/EmuCapture.{cpp,h}` | 210 |
| Build-phase smoke (`--build-smoke`) | `src/BuildSmoke.{cpp,h}` | 162 |
| Harness CLI dispatch for intermediate modes (~40 % of file) | `src/main.cpp` | 163 |
| Classify-dump render + twin stub view (~30 % of file pair) | `src/Classify.{cpp,h}` | 110 |
| Registry surface dump (`--surface`) | `src/Surface.{cpp,h}` | 99 |
| Phase-2 shadow writer + its flags | `ton-trace-emulator/src/TraceInserter.{h,cpp}`, `main.cpp` | 99 |
| **A total** | | **4 925** |

*(The postgres-side shadow actor — `MchClassifierActor` 220 LOC + ~25 LOC of wiring in
`index-engine` / `ton-index-postgres` — is also class A but is counted in §5 as a dilution point,
not here, to avoid double-counting against the engine denominator.)*

### Class B detail

| item | files | now | after |
|---|---|---:|---:|
| **kvrocks tier-2 stack → synchronous celldb fetch + detectors** | 7 units, below | **2 435** | **~860** |
| ↳ suspension unit gate | `src/Tier2SuspendTest.{cpp,h}` | 499 | 0 |
| ↳ payload decode | `src/KvrocksDecode.{cpp,h}` | 551 | ~250 |
| ↳ decode gate | `src/KvrocksDecodeTest.{cpp,h}` | 417 | ~150 |
| ↳ trace-level fixpoint driver | `src/ClassifyDriver.{cpp,h}` | 349 | ~60 |
| ↳ suspension in the actor (262/853 cpp + ~60/190 h) | `src/emu/EmuClassifierActor.{cpp,h}` | 322 | ~40 |
| ↳ blocking-MGET actor pool | `src/emu/EmuLookupActor.{cpp,h}` | 235 | 0 |
| ↳ `--mch-kvrocks*` flags + config | `ton-trace-emulator/src/main.cpp:157-193,237-246` | 62 | ~10 |
| ↳ *new* celldb source + DeDust pool detector | — | 0 | ~350 |
| **Runtime IR → full codegen (EM4)** | `src/IrLoader.*`, `src/ExprEval.*` | **907** | **~300** |
| **Dual interpreter/generated conformance runners** | `src/VectorRunner.*`, `src/BuildVectorRunner.*`, `src/MsgVectorRunner.*` | **775** | **~450** |
| **Schema/prod A/B gate** | `src/SchemaGate.{cpp,h}` | **341** | **~170** |
| **B total** | | **4 458** | **~1 780** |

### Class D detail

| C++ item | files | C++ LOC | Go change | Go LOC |
|---|---|---:|---|---:|
| `float_text()` — CPython `repr(float)` reproduction | `src/emu/EmuActionSerialize.cpp:63-104` | 42 | global `msgpack.Register` widening (below) | — |
| Per-composite int-key exemption machinery (`self_int`, `int_keys`, `jvault_stake_ints`, `layerzero_packet_ints`, `pack_int`) | `:106-116,239-284,341-352` | ~45 | same | — |
| Two-level presence rule (`omitted()` + "every composite key, unfilled as nil") | `:37-61` + `pack_fields` | ~37 | none needed — Go tolerates both already | 0 |
| `mc_seqno_end` written "for parity" | `:330-332` | 2 | none — just stop writing it | 0 |
| — | | | **global int coercion** `msgpack.Register` in an `init()` | +40 |
| — | | | `Flags *FlexInt64`, drop the hand-rolled `ParseInt` | −7 |
| — | | | default `TraceExternalHashNorm` to the HSET key | +3 |
| — | | | `normAddr` at `ptrAccountAddress` (+3 direct sites) | +14 |
| **D total** | | **126** | all in `index/emulated/models.go` | **~50** |

---

## 2. Per-area detail

### 2.1 The twin gate set — which stages are intermediate

`mch-cpp-plan.md` §1 lists twelve gates. Four diff *intermediate pipeline stages* byte-for-byte
against a Python twin; only two diff the actions stage.

| gate | engine mode | twin | stage | class |
|---|---|---|---|---|
| Msg parsing | `--parse-dump` | `py_msg_dump.py` (189) | **intermediate** — parsed message fields | A |
| Match walker | `--match` / `--match-gen` | `py_match_dump.py` (184) | **intermediate** — matcher/block-tree walk, 328 lines | A |
| Classify | `--classify` | `py_classify.py` (306) | **intermediate** — classified blocks before serialization | A |
| Action rows | `--actions` | `py_actions.py` (402) | **actions** — 277 rows / 165 fixtures | A (twin) / C (mode) |
| Write-back | `--actions-msgpack` | `py_actions_msgpack.py` (62) + `cmp_actions_msgpack.py` (217) | **actions**, wire form | A (twin) / C (mode) |
| Schema loader | `--classify-schema` / `--actions-schema` | *self* | mixed | B |
| Prod path | `--classify-prod` / `--actions-prod` | *self* | mixed | B |

**The twins are all class A, including the actions twins.** They import the real Python classifier
(`indexer.events.mch_ir`, `block_tree_serializer`) — `twins/README.md` line 4 — so none survives
Python. That includes `py_actions.py`: the corpus expectations it produces are worth freezing to
disk as goldens, but the generator dies with its import.

**`--parse-dump` / `--match` / `--classify` exist only to have something to diff.** No production
path renders a parse dump or a block-tree dump. `ParseDump.cpp` (207) and `MatchRunner.cpp` (181)
are dump adapters over the core; `Classify.cpp`'s `emit_action` (`:39-66`) is the classify-stage
render.

**`--surface` exists for the twins and nothing else.** `Surface.h` says it: the twins used to
hand-mirror `CPP_PARSERS`/`CPP_PREDS`/`CPP_FNS`/`CPP_SHAPERS`/`CPP_LOOKUP_KINDS`/`BOC_FIELDS` inline
and drifted; `--surface` emits them from the live registries so `cpp_surface.py` can load one source
of truth. The consumer is Python *matcher-runnability* gating — skip-set parity, not an actions
concern. `Surface.{cpp,h}` (99) + `cpp_surface.py` (33) + `cpp_surface.json` +
`indexer/tests/mch/test_cpp_surface.py` go together.

**`py_consumed.py` (187) + `LookupKeyGate` (270) are one gate in two languages.** `LookupKeyGate.h`
states the claim: *"the C++ collect pass demands a SUPERSET of what Python actually consumed, which
is what 'do not port address_selectors' rests on."* It compares which *lookups* the two engines
issue, never which actions they produce — the clearest case in the codebase of a gate with no
meaning once the reference is action rows. `py_consumed.py` even carries bespoke `_internal`
suppression to hide the jvault resolver's inner reads so the two key sets are comparable at all.

**`ab_replay_diff.py` (320) + `mk_hset_dump.py` (64) + `mk_replay_dump.py` (91) + `abreplay/`
(1 238) are the phase-1 A/B apparatus.** `ab_replay_diff.py` compares the payload Python wrote in
prod Redis against the payload our writer produces, bucketed by the `em2-writeback-design.md` §8
taxonomy. `mch-ab-replay` is an entire second executable that exists to produce *our* side from a
prod HSET dump, and `RedisReplay.cpp` (663) is a hand-written mirror of `Serializer.hpp`'s msgpack
structs — including a six-variant `AddressInterfaces` mirror forced by `msgpack-utils.h`'s
unconditional `throw` on `td::RefInt256`. **1 713 LOC, class A.**

If offline replay of *real* traffic is wanted after Python retires, re-derive it from the capture
path as a fixture recorder (§4 G4), not from a Python-shaped dump format.

### 2.2 The kvrocks tier-2 stack — the biggest single item

**The premise.** `em2-tier2-design.md` D3: *"Data source: **kvrocks only** (lead-confirmed), over the
Redis protocol, demand-driven `MGET` on `ton-index:v1:<table>:<raw-addr>:payload`."* D4 routes
`dedust_pool` to the kvrocks `dex_pools` table and records the consequence: *"this removes the
classification reason to add a `DedustPoolDetector` to the emulator."* §6.1 fixes the target `Value`
shapes as *"`ParsedBlockLookupSource`'s tier-1 shapes (the A/B parity contract)"* and, for the
tier-2-only kinds, *"`FixtureLookupSource`'s"* — shapes defined by the Python fixture corpus.
`KvrocksDecode.h` mirrors Python's `destroyed`-means-absent rule per table
(`interface_repository.py:126-127`). D6 (no cross-trace cache) is argued from Python semantics.

Fair to the decision as taken: §6.4's stated rationale is infrastructural (production runs kvrocks,
the C++ client exists, a PG backend would add a `libpqxx` link). But the reason the data is not
simply read *locally* is that the interface tables are what Python's interface repository reads, and
the emulator's own detector set was accepted as insufficient rather than extended. Choose a local
source and the chain collapses. **This is the clearest instance of principle 1: the cheapest fix
lives in a different component (the emulator's detector set) than the one that grew the code.**

**What follows mechanically from "the fetch is a network round trip".** Because
`LookupSource::fetch` is synchronous and buried in `BuildDriver.cpp:243-245`, a network fetch cannot
happen there, so §4 builds a **trace-level collect/fetch/re-classify fixpoint** (`ClassifyDriver`,
349), §5 makes the classifier actor **suspendable** with an id-keyed pending map, a per-`trace_id`
serialization queue, an alarm-driven **deadline sweep** (§5.5), a **circuit breaker** (§5.6) and a
**separate suspended cap** (§5.7) — 262 of 853 cpp lines in `EmuClassifierActor` (`make_fixpoint`,
`start_or_queue`, `run_pass`, `suspend`, `take_pending`, `on_lookup_batch`, `on_lookup_failed`,
`commit_pass`, `release_trace`, `breaker_open`, `note_fetch_ok`, `note_fetch_failed`,
`sweep_pending`, `arm_sweep`) plus ~60 of 190 header lines. §5.3 adds an actor pool with hand-tuned
redis++ options because *"the defaults are a deadlock"*. §5.4 re-defines the deadline and budgets
~800 ms of added tail latency. §4.3 opens a determinism audit — two gaps, one fixed — that exists
only because the trace is classified more than once. Then the tests: `Tier2SuspendTest` (499) drives
a real actor on its own scheduler; `KvrocksDecodeTest` (417) gates the JSON→`Value` mapping.

**The redesign.** A celldb (rocksdb) read of the account state plus the existing smc-interface
detectors is *local and synchronous*. It fits behind `ParsedBlockLookupSource::Tier2Hook` exactly
where the empty `std::function` sits today (`ParsedBlockLookupSource.cpp:149`), with no change to
`BuildDriver`'s inner two-phase loop. That deletes, in order of size: `Tier2SuspendTest` (499);
`EmuLookupActor` (235); the fixpoint (349 → ~60 — the inner per-build fixpoint already handles
lookup-arg-of-lookup, and a synchronous source can satisfy in-pass what today needs a round); the
actor's suspension machinery (322 → ~40, restoring em1-design §16.3's original assumption that *"the
classifier does no I/O on this path"*); `KvrocksDecode` (551 → ~250, replaced by
detector-result→`Value` adapters, which `emu/EmuInterfaces.cpp` already does for the in-trace case
in 80 LOC over six variants); `KvrocksDecodeTest` (417 → ~150); the flags (62 → ~10).

**Net: 2 435 → ~510 existing + ~350 new ≈ 860.**

Two things the redesign must not lose, both class C and source-independent: `normalize_raw_address`
case handling (`Value.cpp:225-258`; tier 1 is case-insensitive by accident, so a raw-arg mismatch is
a silent 100 % miss), and the `jvault_assets` **chain** — three dependent reads that the engine sees
as one lookup (D5). The chain stays; what goes away is that each link was a network round trip.

### 2.3 Capture and the shadow-write A/B path

`EmuCapture.{cpp,h}` (210) writes one JSON file per emission holding the **converter inputs** (raw tx
BOCs + adapted interfaces) so a later offline run re-executes `emu_to_schema_txs` identically
(`em1-design.md` §7). **Class A as built** — but it is the one A item worth *replacing* rather than
deleting: a real-traffic fixture recorder is what the future actions-parity gate needs (§4 G4).
~60 LOC of it survives in spirit, in a separate tool (§5).

`--mch-actions-only` / `ActionsOnlyInsertManager` (`TraceInserter.h:58-82`, `.cpp:441-492` = 77)
plus `--mch-actions-field` / `--mch-aai-prefix` (`main.cpp:195-203` + validation `:252-264` = 22)
are the `em2-writeback-design.md` §8 **phase-2 shadow write**: write our payload into `actions_mch` /
`_aai_mch:` alongside Python's real `actions`, publish nothing, so the writers can be compared on
live traffic. `main.cpp:258-263` refuses to start with production field names. **Class A, 99 LOC**,
and a §5 dilution point — it lives inside the product insert TU.

`--mch-classify-nonblocking` (`EmuShadow.cpp:183-199`) discards the payload and is documented as
*"a perf-measurement mode, never a write-back one"* — dev scaffolding rather than Python parity;
a dilution point (§5), not counted in A.

### 2.4 Serialization — with Go adjustable, most of this becomes class D

**REAL CONTRACT, and cheapest kept where it is — class C:**

- **msgpack array of map-encoded actions**, gzip optional and sniffed by magic
  (`emulated/models.go:750-765,826`).
- **`action_id` byte-identical to the `_aai` ZSET member suffix.** `crud/pending.go:74` compares
  `string(action.ActionId)` to the substring after the single `:` in the member; member format
  `"<trace_key>:<action_id>"` (`emulated/repository.go:88-111`). Changeable in principle, but the
  writer and reader agree cheaply today — no reason to touch it.
- **`_aai` scores are fetched and never inspected** (`z.Score` unused; `GetActions` re-sorts by
  `end_lt`). Python writes `trace.start_lt`; reproducing that specific number is **not** a contract.
  Write anything monotonic.
- **`ancestor_type`** becomes load-bearing the moment child actions are ported
  (`crud/pending.go:150` drops an action whose ancestor types intersect the requested set). Empty
  today because mch emits no child actions. Worth revisiting *in Go* when children land — suppressing
  by `parent_action_id` presence would be simpler than a type-set intersection.
- **Presence of the `actions` key** flips `classification_state` to `"ok"` (`models.go:838,843`).
- **Array order within fields**: `tx_hashes` drives `transactions_full` order;
  `claimed_jettons[i]`↔`claimed_amounts[i]` pair positionally. The actions array order is *not* a
  contract (re-sorted by `end_lt`, non-stable `sort.Slice`), and map key order is irrelevant.
- **`finality` in Go's 0/1/2 enum — and Go is already right.** The number on the wire is the
  *emulator's* enum (`TraceEmulator.h:15-19`, `Emulated/Confirmed/Finalized`), which Python passes
  through raw (`event_deserializer.py:112` assigns the int with no `FinalityState(...)` coercion).
  Python's own 4-value `FinalityState` (`database.py:140-144`, `signed=2, finalized=3`) is a
  **server-side mislabel that never reaches the wire**. No Go change; if the labels bother anyone,
  fix Python. Recorded because rev 1 flagged this as a risk and it is not one.

**CLASS D — fix in Go, delete the C++ (details, since this is the new material):**

The root defect is structural and it already breaks production *today*, with Python as the writer:
`convert_numerics_to_strings` (`database.py:147-174`) stringifies every int except a **top-level**
exclusion set, so ints *inside* composites arrive as msgpack strings while Go declares 16 of them as
`*int64`/`*int32`/`*uint64`. `vmihailenco/msgpack/v5` errors on str→int; one field error aborts the
whole `Unmarshal`, `ConvertHSet` errors, and **the entire trace vanishes from the pending endpoints**
(`crud/pending.go:181-185`). The C++ writer's answer was to hand-maintain a per-composite exemption
list (`jvault_stake_ints`, `layerzero_packet_ints` — `EmuActionSerialize.cpp:252-254`) covering the
two composites mch has ported, leaving the other seven broken for whoever writes them.

**Exactly which composites, since the count has been quoted three ways.** Nine composite kinds
contain a field Python stringifies and Go declares numeric (`em2-writeback-design.md` §2.6, verified
against `emulated/models.go`):

| composite | field(s) | Go type | covered by mch's exemption list? |
|---|---|---|---|
| `jvault_stake_data` | `period` | `*int64` | **yes** (`EmuActionSerialize.cpp:252`) |
| `layerzero_packet_data` | `src_eid`, `dst_eid`, `nonce` | `*int32`,`*int32`,`*int64` | **yes** (`:253-254`) |
| `multisig_create_order_data` | `creator_index`, `expiration_date` | `*int64` | no |
| `multisig_approve_data` | `signer_index`, `exit_code` | `*int64`,`*int32` | no |
| `multisig_execute_data` | `expiration_date`, `approvals_num` | `*int64` | no |
| `layerzero_send_data` | `send_request_id`, `native_fee`, `zro_fee` | `*uint64` | no |
| `layerzero_dvn_verify_data` | `nonce` | `*int64` | no |
| `cocoon_worker_payout_data` | `worker_state` | `*int64` | no |
| `cocoon_unregister_proxy_data` | `seqno` | `*int64` | no |

So: **9 affected, 2 covered, 7 still dropping whole traces whenever Python writes them.** (The brief
said 5 — that number predates the two cocoon composites, which were added later and inherit the same
defect.) The Go fix below covers all nine and any future tenth without anyone remembering to extend
a list, which is the structural argument for it over the exemption approach.

**There is a one-place Go fix that covers all of it.** `msgpack.Register` (`types.go:44-55`) is
exported and keyed on `reflect.Type`; `getDecoder` consults `typeDecMap` first
(`decode_value.go:49-56`) and `ptrValueDecoder` routes `*T` through it (`:60-64,126-140`) with `Nil`
handled before the custom func runs. So an `init()` in `index/emulated` registering a
peek-and-branch decoder for the ten int kinds makes **every** numeric field str-tolerant at once —
the 16 broken composite fields, the 9 top-level fields, and the 43 trace-node numeric fields —
with **zero other edits**: no type changes, no casts, nothing in `index/parse` or `index/models`.
~40 LOC. Verified available on the public API: `PeekCode()` (`decode.go:586`), `DecodeString()`
(accepts str *and* bin, `decode_string.go:19-34`), `DecodeInt64/DecodeUint64/DecodeFloat64`.

Caveats, all manageable: `typeDecMap` is keyed on the exact type, so *defined* types
(`models.FinalityState`, `emulated.AccountStatus`) keep the stock decoder — both arrive as ints
today, and each can be registered explicitly if ever needed; registration must run in `init()`
because `ptrValueDecoder` captures the element decoder at construction and struct fields are
memoized; it is process-global, which is fine here — the only two `msgpack.Unmarshal` call sites in
the repo are `emulated/models.go:791` and `:826`.

**Backward compatibility: strictly widening.** Identical behaviour on every int-coded value, so
existing Python-written blobs decode exactly as they do now, and the fix lands *before* any writer
change. It also fixes the seven composites Python breaks today — a production bug, not a migration
concern.

**The blocker rev 1 missed, and the reason to do the symmetric half too.** `models.go:202-528`
declares **169 `*string` fields** that only work because Python stringifies everything outside the
exclusion set. If a C++ writer emits `amount`, `query_id`, `nft_item_index`, `full_price`, … as
native ints, every one becomes a hard decode error and drops the trace. So either the C++ writer
keeps reproducing Python's stringify rule for 169 keys — exactly the mimicry this audit exists to
remove — or Go gets the mirror-image registration for `string` (~15 LOC, same mechanism, also
strictly widening). **Do the symmetric one.** With both halves registered, the writer's rule becomes
"emit the natural type; 256-bit values as decimal strings because they do not fit an int64", which
is a value-domain fact rather than a Python quirk.

Once that is in, the following C++ deletes:

| C++ | LOC | why it goes |
|---|---:|---|
| `float_text()` — CPython `repr(float)` (`"1037.0"`, `"1e+16"`) | 42 | never a decode contract (all such fields are `*string` passthrough); with `FlexString` registered, the writer can emit a native float or an int and Go renders it |
| `self_int` / `int_keys` / `jvault_stake_ints` / `layerzero_packet_ints` / `pack_int` | ~45 | the exemption list has no purpose once Go accepts both |
| `omitted()` two-level presence rule | ~37 | Go already tolerates both absent and nil (no `DisallowUnknownFields` anywhere); pick one rule, ~10 LOC |
| `mc_seqno_end` "kept for parity, one key" | 2 | Go has no field |

**Three more small Go changes, each retiring a C++ or Python obligation:**

- **`change_dns_record_data.flags`** — `models.go:1056-1063` hand-parses it with
  `strconv.ParseInt(..., 10, 64)` and returns the error, which propagates out of `FillFromRawData`
  as a **500 for the whole request** (`main.go:2008-2011`), a *worse* failure than the per-trace
  drop. Python can already emit `"1.0"` here (`block_tree_serializer.py:572,577` via
  `str(float)`), so this is live. Change the field to the flexible int type and delete the parse:
  **Go −7 LOC**, removes an outage class.
- **`trace_external_hash_norm`** — `models.go:929-932` surfaces it only when it differs from
  `trace_external_hash` (forced to the HSET's `root_node` at `:834`). `ConvertHSet` already receives
  the HSET key as an otherwise-unused parameter (`:767`), and `repository.go:22-64` guarantees that
  key is the real one. Defaulting the field to it when absent is **+3 LOC** and lets the writer drop
  an always-written, always-derivable key from every action of every trace. Note the correct default
  is the **HSET key** (normalized hash), not `root_node` (raw hash) — they differ except where
  normalization is a no-op.
- **Address casing** — Go inlines payload addresses verbatim into `WHERE las.account IN (…)`
  (`crud/crud.go:368-390`). A wrong-case address fails *silently*: the addressbook join misses, the
  fallback branch still returns a row with `code_hash = nil`, and the client gets `UQ…` instead of
  `EQ…` with `interfaces: []`. `ptrAccountAddress` (`models.go:869-871`) is the single choke point —
  70 call sites funnel through it; three direct conversions bypass it (`:945`, `:1163`, `:1192`).
  A cheap uppercase-the-hex-after-the-colon normalizer there is **+14 LOC** and a no-op on every
  existing payload. Do *not* route this through `ParseAccountAddressStruct` — it makes three parse
  attempts per address on a hot path.

**Total class D Go work: ~50 net LOC, all in `index/emulated/models.go`, all backward compatible.**
It retires 126 LOC of C++ and — worth more — removes "reproduce Python's stringify rules" from the
design constraints entirely.

`EmuActionSerializeTest.cpp` (817) asserts on the **decoded map**, never on bytes, so it is a
Go-contract gate rather than a Python-parity gate. **Class C, keep in full** — it matters more once
the twin comparators go. Only the assertions pinning `repr(float)` text and the
top-level-omit-vs-nil distinction need revisiting alongside the rows above.

### 2.5 The runtime IR artifact

`mch_ir.json` (273 KB) is loaded at runtime by `IrLoader` (635) into `CompiledMatcher`/`CompiledNode`
tables, and `ExprEval` (272) tree-walks the IR expression encoding. Both are live in production:
`ClassifyCore` never passes a `WhereTable`, so `Walker::eval_where` takes the interpreter branch
(`Walker.cpp:105-133`); the generated where table is reachable only via `--match-gen` /
`--vectors-gen`.

The dual execution is a design rule. `mch-cpp-plan.md` §0.2: *"The interpreter (`ExprEval`) and all
generated code call the same `rt_*` functions… If codegen output disagrees with the interpreter,
codegen is wrong."* §0.3: *"Python is ground truth."* §0.7: *"IR JSON is the version-controlled
contract. Python emits it."* Shipping the artifact at runtime is what makes **both engines execute
the same document**, which is what makes a stage-wise byte diff meaningful.

EM4 already plans the exit (`emu-integration-plan.md` §3). Under actions-only parity the stronger
option loses its main objection, because no Python side must load the same file.

- **Covered by codegen:** build programs. `BuildDriver` selects `gen_builds_ir()` by artifact sha;
  there is no build-program interpreter in production at all.
- **Covered but unused:** `where_expr` functions (`gen_wheres()`). One of the two paths is redundant.
- **Not covered:** the matcher layer — pattern trees, captures, registration order, skip
  computation. That is `IrLoader`'s ~500 lines of JSON parsing.

**Redesign:** emit the matcher tables from `mch-codegen` (≈ +300 LOC in `codegen/`, already 1 010
LOC and structured for it), delete `IrLoader` (635) and `ExprEval` (272), drop `--mch-artifact` or
demote it to a dev override. **907 → ~300**, and the startup-fatal artifact load
(`ton-trace-emulator/src/main.cpp:274`, `ton-index-postgres/src/main.cpp:629-638`) and the
sha-pairing guards simplify with it.

**Knock-on:** `VectorRunner` (313) runs `expr_vectors.json` through the interpreter and
`--vectors-gen` through generated code; `BuildVectorRunner` (348) and `MsgVectorRunner` (114) are the
same shape. With one execution path the dual runners collapse and the vectors become frozen goldens
that still earn their keep. **775 → ~450.**

### 2.6 SchemaGate / SchemaTraceLoader / TraceLoader

**`TraceLoader` (412) — class C by function, but misplaced (§5).** Loads the `.lz4` fixture corpus.
Reading a frozen corpus is not mechanism parity; compiling the fixture reader into the *production*
classification library is a dilution point.

**`SchemaTraceLoader` (185) — class C.** The real data path. Keep.

**`SchemaGate` (341) — class B.** `--classify-schema` routes each fixture through
`msgpack → our structs → schema → SchemaTraceLoader → our structs` and diffs against `--classify` —
a C++-internal equivalence check. But `--classify-prod` / `--actions-prod` additionally **synthesize
tier-1 interfaces** from the fixture's interface dict (`synth_account_interfaces`, `:186-284`) **and
install a fixture-backed tier-2 hook over the Python-gathered superset** (`GateProdLookupSource`).
`em2-tier2-design.md` §2 is blunt: *"The 165-fixture gates are blind to all of it:
`SchemaGate.cpp:265-269` installs a fixture-backed tier-2 hook over the Python-gathered superset…
They prove the shape of the two-tier split and nothing about sourcing."* That superset is what
Python's eager `address_selectors` *predicted* it might need — a Python mechanism reproduced in C++
so the gate can run. Keep `--actions-prod` over the corpus; drop the `--classify-*` block-tree diffs
and the synthesis that exists only to make an intermediate diff possible. **341 → ~170.**

### 2.7 Host-fn behaviours shaped by Python

**`HostTonco` router-wallet miss — already fixed *away* from Python, and it is the model.** The
ported behaviour was `if (first_asset.is_null()) first_asset = Value::make_asset_ton();` — a missing
counterparty wallet published as a **TON deposit**. `em2-tier2-design.md` §8.2 calls it *"the single
site in the engine that answers a miss with a plausible wrong value rather than with nothing"*; W1
replaced it with `return rt_ok(Value::null())`, which `tonco_liquidity.mch:59-60`'s existing
`reject when r == null` turns into a clean action-scoped rejection (`HostTonco.cpp:336-350`).
Exactly the trade the new criterion endorses. **Class C (keep the fix)**; the item of note is that
the original shape was Python-mirroring.

**`HostDedust` determinism — motivated by re-classification, not Python.** `HostDedust.cpp:154-171`
previously collected `notif_blocks` from an `unordered_set`, making the `stable_sort` tie order
address-dependent; W1 made it iterate the ordered `consumed` vector. **Class C** — correct
regardless; if the fixpoint goes away (§2.2) it simply stops being load-bearing. **Gap 2 is still
open**: `ClassifyCore.cpp:204` sorts `children_blocks` by pointer value, which reaches action output
through `unwind_deployments` and the fallback rows. That is a latent non-determinism *in the actions*
and is worth fixing under the new criterion.

**No "match Python dict ordering" logic exists in the host layer.** The one ordering concession is
the opposite: composite maps are packed in insertion order and the comparator is explicitly
order-insensitive (`cmp_actions_msgpack.py`, `em2-writeback-design.md` §2.7/§11);
`tx_hashes` is sorted C++-side, unsorted Python-side, and listed under `ORDER_INSENSITIVE` in
`ab_replay_diff.py`. Nothing to remove.

---

## 3. What is *not* mechanism parity (guardrails)

- **`ActionBuild.{cpp,h}` (926)** — the per-btype fills. This *is* actions parity.
- **The `unknown` synthetic row** (277 rows = 227 matched + 50 unknown) — Python's
  `create_unknown_action` behaviour, but an *output* the pending endpoints depend on.
- **`basic_classify_fallback`** (`ClassifyCore.cpp:353-361`, dormant) — same reasoning;
  `emu-integration-plan.md` §3 calls it a flip blocker for good reason.
- **The actions finality guard** (`mch::actions_write_is_stale`, `TraceInserter.cpp:361-408`) —
  justified partly by the Python-writer A/B window, but re-emissions at rising finality race
  regardless of who writes, and it mirrors the per-node guard at `TraceInserter.cpp:88-95`. Keep.
- **`EmuTraceConvert` / `EmuInterfaces` / `EmuShadow` / `EmuTypes` (643)** — the in-process seam,
  the gate, the work cap, the cell-anchor lifetime.
- **`ParsedBlockLookupSource` (266)** — the two-tier source itself; only its tier-2 backend changes.

---

## 4. Proposed future acceptance criterion

Four gates replace twelve. Organising principle: **gate the output, gate it on real inputs, stop
gating the stages in between — and build every gate in a target the product does not link.**

### G1 — Fixture actions gate (the workhorse)
`--actions` and `--actions-msgpack` over `tests/traces`, diffed against **committed goldens**, not a
live Python twin. Generate the goldens once from today's green `py_actions.py` run (277 rows / 165
fixtures) and freeze them; thereafter they are edited by review like any other expectation file.
**Target:** harness exe + `mch-fixtures` (§5). Retires `twins/`, `--surface`, `--parse-dump`,
`--match`, `--classify`, `--build-smoke`, `--lookup-keys`.

### G2 — Wire-contract self-test
`--actions-msgpack-test` kept and extended — after the class-D Go changes it gets *smaller*, because
the int/string split stops being a contract. What remains to assert: `action_id` ↔ `_aai` member
suffix, canonical addresses, std-base64 hashes, `finality` in the emulator's 0/1/2, and that the blob
decodes at all. **A round-trip through the real Go decoder** (a `go test` fixture consuming a blob
the C++ test emits) is strictly better than a C++ mirror of Go's expectations, and is the natural
home for this gate once Go is a place we change. **Target:** harness exe + a small Go test.

### G3 — Production-path actions gate
`--actions-prod` over the corpus, diffed against G1's golden. Drops the `--classify-*` block-tree
diffs and the Python-superset interface synthesis (§2.6). Add what today's gates structurally cannot
see: a **tier-2 sourcing gate** running the real celldb-backed source against a small recorded
account set, asserting kind→`Value` shape equality with tier 1 — `KvrocksDecodeTest`'s job,
re-pointed (~150 LOC). **Target:** harness exe.

### G4 — Live actions A/B, on action rows only
Replaces the phase-1/phase-2 apparatus. Compare the classifier **against itself across versions**:
record real emulated traces (the `EmuCapture` successor — write fixtures, not JSON debug dumps),
replay through old and new binaries, diff the action rows. Buckets shrink from nine to two: *changed
row*, *row count changed*. Retires `ab_replay_diff.py`, `mch-ab-replay`, `mk_hset_dump.py`,
`mk_replay_dump.py`, `--mch-actions-only`, `--mch-actions-field`, `--mch-aai-prefix`.
**Target:** a separate `mch-record` tool + the harness exe; nothing in the product path.

**One thing is unrepeatable.** Run the existing phase-1 diff once on a real prod sweep *before*
Python retires and archive the traces where the two engines disagree. That is the only chance to
capture "Python classified this differently" at scale, and it seeds G4.

### No longer gated, deliberately
Message parsing, the matcher walk, the classified block tree, the lookup key set, matcher skip sets,
interpreter-vs-codegen equivalence, Python↔C++ byte identity at any stage. Each was a real
bug-catcher; the trade is accepted because a wrong intermediate that produces the right action rows
is not a defect under the new criterion, and the corpus + G2 catch the ones that are.

---

## 5. Dilution inventory — harness concerns inside product code

Standing principle: **harness / shadow / A-B machinery lives in separate build targets; product-code
touches get a `TODO: remove (harness)` marker and are swept before any PR.**

§§5.1-5.6 group the sites by the product artifact they pollute and explain each one. **§5.7 is the
mechanical checklist** — every site numbered `H1…H36`, with file:line, the target it moves to (or
"delete"), and whether an interim marker is needed. **36 sites**: 29 want a `TODO: remove (harness)`
marker now, 7 are pure deletions that need none because they go in the same pass.

### 5.1 Target layout, current vs proposed

| target | kind | linked by | problem |
|---|---|---|---|
| `mch-classify` | product static lib | `ton-index-postgres`, `index-engine`, everything | contains fixture-corpus code and conformance-vector tables |
| `mch-emu` | product static lib | emulator (via bridge), harness | contains the capture writer |
| `mch-emu-bridge` | product static lib | `ton-trace-emulator` | clean (one TU) |
| `ton-mch-engine` | harness exe | — | already separate ✅ but `install()`ed into `bin` |
| `mch-ab-replay` | A/B exe | — | already separate ✅, delete outright (§2.1) |
| **`mch-fixtures`** *(proposed, new)* | **test-only static lib** | harness exe + ctest only | receives `TraceLoader`, `FixtureLookupSource`, the vector tables |
| **`mch-record`** *(proposed, new)* | **small tool** | — | receives the `EmuCapture` successor |

### 5.2 Sites in `mch-classify` (the library the postgres indexer links)

| site | what | action |
|---|---|---|
| `CMakeLists.txt:118` → `src/TraceLoader.cpp` (412 LOC) | the `.lz4` **fixture** reader, in the production classification lib | move to `mch-fixtures` |
| `src/BuildRuntime.h:131-158` + `src/BuildRuntime.cpp:207-392` (~214 LOC) | `FixtureLookupSource` — the fixture-corpus lookup source, **including the jvault fixture chain** (`:310-392`) whose header comment documents Python's `TestInterfaceRepository` semantics | move to `mch-fixtures`; keep the `LookupSource` interface in the lib |
| `CMakeLists.txt:99-101` → `vectors_generated.cpp`, `builds_vectors_generated.cpp` in `MCH_GENERATED_SOURCES` | conformance-**vector** generated tables (from `ir/expr_vectors.json`, `ir/build_vectors.json`) compiled into the production lib | split `MCH_GENERATED_SOURCES` into prod (`wheres`, `builds_ir`, tlb) and test (`vectors`, `builds_vectors`) sets |
| `CMakeLists.txt:73-86` + `src/BuildsTestbedStub.cpp` | `-DMCH_TESTBED_IR` testbed knob; the stub TU is always compiled into the lib | move the testbed table into `mch-fixtures`, or drop the knob |
| `src/KvrocksDecode.cpp` (551) at `CMakeLists.txt:150` | tier-2 payload decode placed in the classify lib **specifically so it could be gated with no store** (`KvrocksDecode.h` head: *"That split is what lets the whole mapping be gated offline"*) — a testability-shaped placement | resolved by §2.2's redesign; the replacement belongs with the source, wherever that lands |
| `src/ExprEval.cpp`, `src/IrLoader.cpp` | the "reference/fallback" interpreter path | deleted by §2.5 |
| `src/MchClassifierActor.cpp` (154 + 66 h) | **shadow-only** actor: *"It writes NOTHING — no Action rows, no DB, no effect on the pipeline"* | delete with its wiring (§5.5) |

### 5.3 Sites in `mch-emu`

| site | what | action |
|---|---|---|
| `CMakeLists.txt:203` → `src/emu/EmuCapture.cpp` (210) | JSON dump of converter inputs for offline replay | move to `mch-record` as a fixture writer |

### 5.4 Sites in `ton-trace-emulator` (the production binary)

| site | what | action |
|---|---|---|
| `src/main.cpp:99-101` `--mch-capture` | capture dir | move with `EmuCapture` |
| `src/main.cpp:103-105` `--mch-classify-only` | selects `NullTraceInsertManager` — a run that writes nothing | build-time or dev-only |
| `src/main.cpp:107-109` `--mch-classify-nonblocking` | discards the payload; *"a perf-measurement mode, never a write-back one"* | dev-only |
| `src/main.cpp:111-123` `--mch-stress-backfill` | help text literally says **"TEST ONLY"** | dev-only |
| `src/main.cpp:125-139` `--mch-classify-deadline-ms` | help text says **"TEST ONLY"** | dev-only |
| `src/main.cpp:195-197` `--mch-actions-field`, `:198-200` `--mch-aai-prefix`, `:201-203` `--mch-actions-only` | phase-2 A/B key renaming + shadow write | delete with the A/B (§2.3) |
| `src/main.cpp:252-264` | validation that `--mch-actions-only` refuses production key names | delete with the above |
| `src/main.cpp:157-193`, `:237-246` `--mch-kvrocks*` | tier-2 transport config | collapses with §2.2 |
| `src/TraceInserter.h:47-56` `NullTraceInsertManager` | classify-only sink | dev-only |
| `src/TraceInserter.h:58-82` + `src/TraceInserter.cpp:441-492` `ActionsOnlyInsertManager` / `ActionsOnlyInserter` (77) | the phase-2 shadow writer, **inside the product insert TU** | delete (§2.3) |
| `src/TraceInserter.h:17-22` `ActionWriteConfig::{actions_field, aai_prefix, finality_field()}` | field names parameterised **only** so the A/B could rename them | inline the constants |

### 5.5 Sites in `ton-index-postgres` / `index-engine` (the indexer product)

| site | what | action |
|---|---|---|
| `ton-index-postgres/src/main.cpp:57` field, `:280-282` `--mch-shadow` flag, `:629-638` startup-fatal artifact load | shadow-mode entry point in the production indexer | delete with `MchClassifierActor` |
| `index-engine/src/IndexScheduler.h:18` include, `:103-105` `set_mch_shadow` | shadow hook on the production scheduler | delete |
| `index-engine/src/IndexScheduler.cpp:528` | `create_actor<mch::MchClassifierActor>("MchShadow", …).release()` fired per seqno | delete |

~25 LOC of wiring + 220 LOC of actor. It was the Phase-G way to observe the C++ engine on
production data; G4 (§4) covers the same need from a separate tool.

### 5.6 Smaller seams worth marking

| site | what |
|---|---|
| `src/Classify.h:52-60` `TraceContextLoader` / `LookupSourceFactory` | injection seams that exist **only** so `SchemaGate` can substitute a loader and a fixture-backed lookup factory |
| `src/Classify.cpp:121-136` `twin_view()` | the Python-twin stub emission (`trace_external_hash_norm = "twin"`, finality = finalized) inside the engine |
| `src/ClassifyDriver.h:22-27` | documents the "with no `Tier2Fetch`… byte-identical to the pre-driver single-pass code" invariant — a gate-shaped invariant constraining product code |
| `CMakeLists.txt:267` `install(TARGETS ton-mch-engine RUNTIME DESTINATION bin)` | a gate-only harness exe installed into the production bin dir |
| `src/emu/EmuActionSerialize.cpp:37-61,63-104,242-254,286-292` | comment blocks and logic keyed to `database.py` / `block_tree_serializer.py` line numbers — class D removes the logic; the comments go with it |

### 5.7 The mechanical checklist — 36 sites

Marker text throughout: `// TODO: remove (harness) — <target it moves to, or "delete per
python-parity-audit §N">`. "Marker" = **no** only where the site is deleted in the same pass, so a
marker would be dead on arrival.

**In `mch-classify` — the static lib `ton-index-postgres` and `index-engine` link (9)**

| # | site | what | destination | marker |
|---|---|---|---|:--:|
| H1 | `CMakeLists.txt:118`, `src/TraceLoader.{cpp,h}` | `.lz4` **fixture** reader (412) | `mch-fixtures` | yes |
| H2 | `src/BuildRuntime.h:145-157`, `src/BuildRuntime.cpp:219-392` | `FixtureLookupSource` incl. the jvault fixture chain (`:310-392`) (~214) | `mch-fixtures` | yes |
| H3 | `CMakeLists.txt:99-101,105-113` | `vectors_generated.cpp` + `builds_vectors_generated.cpp` inside `MCH_GENERATED_SOURCES` | split into prod/test source sets | yes |
| H4 | `CMakeLists.txt:73-86`, `src/BuildsTestbedStub.cpp` | `-DMCH_TESTBED_IR` knob + always-compiled stub | `mch-fixtures`, or drop the knob | yes |
| H5 | `src/GenBuilds.h:9-20,41-46` | `gen_builds_vectors()` / `gen_builds_testbed()` — test-document tables declared in a product header | `mch-fixtures` header | yes |
| H6 | `src/KvrocksDecode.{cpp,h}` | placed in the classify lib *so it could be gated with no store* (`KvrocksDecode.h` head) | follows the tier-2 source (§2.2) | yes (do-not-extend) |
| H7 | `src/ExprEval.{cpp,h}` | interpreter "reference/fallback" path | delete (§2.5) | yes (do-not-extend) |
| H8 | `src/IrLoader.{cpp,h}` | runtime IR loading | delete (§2.5) | yes (do-not-extend) |
| H9 | `src/MchClassifierActor.{cpp,h}` | shadow-only actor, *"writes NOTHING"* (220) | delete with H27-H29 | no |

**In `mch-emu` — linked by the emulator via `mch-emu-bridge` (3)**

| # | site | what | destination | marker |
|---|---|---|---|:--:|
| H10 | `CMakeLists.txt:203`, `src/emu/EmuCapture.{cpp,h}` | JSON dump of converter inputs (210) | `mch-record` | yes |
| H11 | `src/emu/EmuTypes.h:223-225` | `EmuShadowConfig::capture_dir` — a harness flag threaded through the production config struct | `mch-record` | yes |
| H12 | `src/emu/EmuClassifierActor.cpp:219-224`, `:312-317` | `mkpath` at start-up and `write_capture` **on the classify path** of the production actor | `mch-record` | yes |

**In `ton-trace-emulator` — the production binary (14)**

| # | site | what | destination | marker |
|---|---|---|---|:--:|
| H13 | `src/main.cpp:99-101` | `--mch-capture` | with H10-H12 | yes |
| H14 | `src/main.cpp:103-105` | `--mch-classify-only` → `NullTraceInsertManager` | dev-only build | yes |
| H15 | `src/main.cpp:107-109` | `--mch-classify-nonblocking` (payload discarded) | dev-only build | yes |
| H16 | `src/main.cpp:111-123` | `--mch-stress-backfill` — help text says **"TEST ONLY"** | dev-only build | yes |
| H17 | `src/main.cpp:125-139` | `--mch-classify-deadline-ms` — help text says **"TEST ONLY"** | dev-only build | yes |
| H18 | `src/main.cpp:157-193`, `:237-246` | `--mch-kvrocks*` (6 flags + XOR validation) | collapses with §2.2 | yes (do-not-extend) |
| H19 | `src/main.cpp:195-203` | `--mch-actions-field`, `--mch-aai-prefix`, `--mch-actions-only` | delete (§2.3) | no |
| H20 | `src/main.cpp:226-234` | the "requires `--mch-artifact`" validation block covering the harness flags | shrinks with H13-H19 | yes |
| H21 | `src/main.cpp:252-264` | validation refusing `--mch-actions-only` with production key names | delete with H19 | no |
| H22 | `src/TraceInserter.h:17-22` | `ActionWriteConfig::{actions_field, aai_prefix, finality_field()}` — parameterised **only** so the A/B could rename keys | inline the constants | yes |
| H23 | `src/TraceInserter.h:47-56` | `NullTraceInsertManager` | dev-only build | yes |
| H24 | `src/TraceInserter.h:58-82`, `src/TraceInserter.cpp:441-492` | `ActionsOnlyInsertManager` / `ActionsOnlyInserter` (77) — the phase-2 shadow writer inside the product insert TU | delete (§2.3) | no |
| H25 | `src/TraceScheduler.h:63-64`, `:109-141` | `mch_shadow_` / `mch_classifier_` members and the ctor decoration that threads `nonblocking`, `max_nodes`, `gate` and `stress_backfill` through the production scheduler | keep the seam, drop the harness params | yes |
| H26 | `src/emu/EmuShadow.h:40-42`, `src/emu/EmuShadow.cpp:134-140`, `:186-199` | `decorate_sink`'s `nonblocking` parameter and the `shed_payload("nonblocking")` branch — a perf-measurement mode woven into the production sink decorator | delete with H15 | yes |

**In `ton-index-postgres` / `index-engine` — the indexer product (3)**

| # | site | what | destination | marker |
|---|---|---|---|:--:|
| H27 | `ton-index-postgres/src/main.cpp:57`, `:280-282`, `:629-638` | `--mch-shadow` field, flag, and startup-fatal artifact load | delete | no |
| H28 | `index-engine/src/IndexScheduler.h:18`, `:103-105` | `MchClassifierActor.h` include + `set_mch_shadow` on the production scheduler | delete | no |
| H29 | `index-engine/src/IndexScheduler.cpp:528` | `create_actor<mch::MchClassifierActor>("MchShadow", …).release()` per seqno | delete | no |

**Harness-shaped seams elsewhere (7)** — the harness *exe* is already a separate target, so these are
about seams that reach back into shared or product-adjacent code.

| # | site | what | destination | marker |
|---|---|---|---|:--:|
| H30 | `src/Classify.h:64-70`, `src/Classify.cpp:150-258` | `enum class DumpMode {classify, actions, actions_msgpack}` and `run_dump`'s three-way dispatch — **two of the three modes are the intermediate/twin renders**; the enum comments name the twin file for each | keep `actions*`, delete `classify` (§2.1) | yes |
| H31 | `src/Classify.h:52-60` | `TraceContextLoader` / `LookupSourceFactory` — injection seams existing **only** so `SchemaGate` can substitute a loader and a fixture-backed lookup factory | shrink with §2.6 | yes |
| H32 | `src/Classify.cpp:121-136` | `twin_view()` — the Python-twin stub emission (`trace_external_hash_norm = "twin"`, finality = finalized) | delete with the twins | yes |
| H33 | `src/main.cpp:176-250` | 23 mode flags, of which 9 are intermediate-stage dumps (`--match`, `--match-gen`, `--build-smoke`, `--classify`, `--classify-schema`, `--classify-prod`, `--lookup-keys`, `--parse-dump`, `--surface`) | delete (§4) | yes |
| H34 | `src/ClassifyDriver.h:22-27` | documents the *"with no `Tier2Fetch`… byte-identical to the pre-driver single-pass code"* invariant — a gate-shaped constraint written into a product header | goes with §2.2 | yes |
| H35 | `CMakeLists.txt:267` | `install(TARGETS ton-mch-engine RUNTIME DESTINATION bin)` — a gate-only harness exe installed into the production bin dir | drop the `install()` | yes |
| H36 | `src/emu/EmuActionSerialize.cpp:37-61`, `:63-104`, `:242-254`, `:286-292` | logic and comment blocks keyed to `database.py` / `block_tree_serializer.py` line numbers | delete with class D (§2.4) | yes |

**Correctly separated already, for contrast — do not touch:** `mch-emu-bridge` (one TU),
`ton-mch-engine`'s gate TU list (`CMakeLists.txt:236-255`), `ABI_BRIDGE_TEST_FIXTURES`
(`:229-234`, compiled into the harness exe only, with a comment saying exactly that), and the ctest
set (`:302-328`, all hermetic). The pattern to copy when creating `mch-fixtures` and `mch-record` is
`ABI_BRIDGE_TEST_FIXTURES`: fixture sources named in one variable, referenced by exactly one target,
with the reason stated at the definition site.

---

## 6. Top five wins, ranked

1. **Kvrocks tier-2 → synchronous celldb + `DedustPoolDetector`.** 2 435 → ~860 LOC, and it removes
   suspension, the deadline sweep, the circuit breaker, the suspended cap, the pending map and
   per-trace queue, the actor pool with its hand-tuned redis++ options, the ~800 ms tail budget, and
   the "classify the same trace up to 6× deterministically" problem class. Also the clearest case of
   *the fix belongs in another component*: the emulator's detector set, not the classifier.
2. **~50 LOC of Go retires the entire stringify-mimicry requirement.** One `init()` in
   `index/emulated/models.go` registering flexible int **and** string decoders deletes 126 LOC of
   C++ wire-shape logic, fixes seven composites Python breaks in production *today*, removes a
   whole-request 500 hazard (`flags`), and — the real prize — deletes "reproduce Python's stringify
   rules" from the design constraints. Strictly widening, so it lands before any writer change.
3. **Delete `twins/` + `abreplay/` + `--surface`: 3 464 LOC of pure deletion**, zero redesign risk,
   available the day Python's `--emulated-traces` is off. Precondition: freeze G1's goldens, and run
   the prod A/B sweep once (§4).
4. **Full codegen, drop the runtime IR.** 907 → ~300 LOC, removes `mch_ir.json` from the production
   binary's runtime surface and the startup-fatal load from two products, collapses the
   interpreter/generated dual path and the sha guards, and lets the conformance runners collapse too
   (a further 775 → ~450).
5. **Un-dilute the product targets (§5).** Not a LOC win — a *review* win. Moving `TraceLoader` and
   `FixtureLookupSource` out of `mch-classify`, `EmuCapture` out of `mch-emu`, and deleting the
   `--mch-shadow` / `--mch-actions-only` paths means the production libraries stop carrying fixture
   formats and A/B modes, and the next reader of `mch-classify` sees only the pipeline.
