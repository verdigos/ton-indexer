# MCH TL-B → Value Codegen — Feasibility Study

Status: 2026-07-07. Investigation + design proposal. **No implementation.** Owner
question (user): today every message type needs a hand-written C++ adapter in
`ton-mch-engine/src/parse/*.cpp` producing a `Value` Obj with named fields. Can
TL-B codegen be adjusted/wrapped — just for this module — so a TL-B schema alone
produces a parser emitting name-accessible `Value` fields, reducing hand-parser
work to zero (or near) when TL-B is available?

Short answer: **yes for the clean part of a body, no for the quirky tail — and
almost every real family is clean-head + quirky-tail, so the right unit of
automation is "generate the head, call a named hand hook for the tail," not
"generate the whole parser."** The mechanical work (unpack, per-field
`Ref<CellSlice>→Value` conversion, field-key assembly, Obj/Dict wrap) is a small
fixed table; the residue (pytoniq remainder/copy() quirks, conditional attrs,
nested ref-walks) is irreducible and stays hand-written. Recommendation: a
`mch-codegen --msgs` mode that parses the `.tlb` subset we use and self-emits
Value-Obj unpackers calling the already-A/B'd `PSlice` helpers, dropping the tlbc
dependency for this module, staged behind the existing `--parse-dump` gate.

---

## 1. How tlbc works and what is exploitable

`tlbc` (`external/ton/crypto/tl/`, ~7.5k LOC vendored TON) compiles a `.tlb` into
a namespaced C++ header + cpp. We run it in CMake over `tlb/mch-msgs.tlb` →
`generated/mch-msgs-tlb.{cpp,h}` (namespace `mchmsg::gen`), consumed today by
**only** `ParseJettons.cpp`.

Per TL-B type tlbc emits `struct <Type> final : TLB_Complex` with:

- **`struct Record`** (or `Record_<cons>` for multi-constructor) — named, typed
  members, one per field. This is the exploitable surface.
- **`unpack(CellSlice&, Record&) -> bool`** — fetches all fields in order,
  `false` on tag-mismatch / underflow. The mechanical win: one call fills the
  whole record.
- pack / skip / validate_skip / print / get_tag — unused by us.

**Field member C++ types collapse the TL-B semantics.** The mapping
(`cpp_val_type` in `tlbc-gen-cpp.h`) is:

| TL-B field | generated member type |
|---|---|
| `uintN`/`intN`, N≤64 | native `unsigned` / `unsigned long long` / `int` |
| `^Cell` | `Ref<vm::Cell>` |
| `VarUInteger 16`, `MsgAddress`, `Maybe ^Cell`, `Maybe X`, `bits N` | `Ref<vm::CellSlice>` (a **sub-slice the caller must further decode**) |

So `amount:(VarUInteger 16)`, `destination:MsgAddress`, and
`custom_payload:(Maybe ^Cell)` are **all** `Ref<CellSlice>` in the record — the
semantic type is gone from the member. The only place it survives in the
generated output is a trailing **comment**: `// amount : VarUInteger 16`.

Internally tlbc *does* carry the full structured type
(`CppTypeCode::ConsRecord::cpp_fields` — a `vector<ConsField>` where each
`ConsField` has `name`, `ctype:cpp_val_type`, `size`, and a `Field&` with the
original `TypeExpr`), but it **never emits a machine-readable field manifest** —
only the C++ struct plus the human comment.

Exploitable surfaces, richest first:
1. **tlbc's internal model** (`ConsField{name,ctype,size,TypeExpr}`) — complete,
   but reachable only by *modifying tlbc* (a vendored-TON fork; upstream-sync
   liability).
2. **the generated header field comments** (`// name : TLB-type`) — carry the
   semantic type, but are a *comment* (format-coupled to tlbc's emitter, fragile
   to scrape).
3. **the `.tlb` source itself** — the original truth, and it is *ours* (we wrote
   `tlb/mch-msgs.tlb`; it deliberately mirrors what the Python parsers read, not
   TEP-74).

### How `ParseJettons.cpp` uses it today (what's manual vs mechanical)

```
Record rec; t_MchJettonTransfer.unpack(cs, rec);        // (1) mechanical — tlbc
var_uint16(rec.amount)            -> make_int            // (2) hand conversion
addr_value(rec.destination)       -> account            //     per field type
fetch_maybe_ref(rec.custom_payload)+boc_bytes -> bytes  //
Value::Fields f; f.emplace_back("amount", ...); ...      // (3) assemble by name
Value::make_obj(f)                                       // (4) Obj vs Dict
// then forward_payload TAIL is hand-read entirely       // (5) quirk — the .tlb
//   (the .tlb intentionally STOPS at forward_amount)    //     stops here
```

## 2. Anatomy of an adapter — mechanical vs semantic

Every adapter decomposes into five layers:

1. **unpack prefix → Record.** Mechanical. tlbc does it (or trivial hand fetches).
2. **per-field `Ref<CellSlice>→Value` conversion.** A **fixed, small converter
   table keyed by TL-B field type**, all of which already exist as `PSlice`
   helpers:
   - `uintN`/`intN` (native) → `make_int`
   - `VarUInteger 16` → `var_uint16` / `load_coins_py` → `make_int`
   - `MsgAddress` → `load_address_py` → account Value
   - `^Cell` → `make_cell`
   - `Maybe ^Cell` → `fetch_maybe_ref` → **{`make_cell` | `boc_bytes`→bytes}** ← policy-ambiguous
   - `bits256` → hex string, etc.
   Mostly derivable from the TL-B type. Two things TL-B alone does **not**
   determine: (a) `Maybe ^Cell` → cell-vs-bytes (jetton `custom_payload`→bytes,
   notify `forward_payload`→cell); (b) the **opcode field is dropped** (Python
   skips-without-validating and never surfaces it).
3. **field-key names.** 1:1 from TL-B field names. Mechanical.
4. **Obj vs Dict.** Per-message policy (message bodies → Obj). One bit.
5. **quirky tail / nested hand-reads** (`forward_payload`, `master_msg`
   sub-parse, dedust ref-walk). Hand. Irreducible.

**The automatable envelope is layers 1–4 for the clean prefix — i.e. everything
up to the first quirk.** Real families are **head-clean + tail-quirky**:

| Family | Head (generatable) | Tail (hand) |
|---|---|---|
| `JettonBurn`, `JettonInternalTransfer`, `JettonMint` | whole body | — (fully generatable) |
| `JettonTransfer`, `JettonNotify`, `PTonTransfer` | prefix fields | `forward_payload` remainder/copy() quirk |
| `MinterJettonMint` | prefix | nested `master_msg` hand-read |
| `DedustSwapNotification`, `ChangeDnsRecord`, `LayerZero…` | little/none | ref-walk / conditional attrs (stay hand) |

Current reality: **only 1 of 9 parser TUs uses tlbc** (`ParseJettons`, 283 LOC);
the other 8 are pure hand (1036 LOC). But that's a *history* artifact (jettons
was the tlbc pilot), not proof the rest are unautomatable — `PTonTransfer`'s head
is 3 clean fields, `CoffeeCreateVault` is 54 LOC total. The quirk is usually the
**tail**, not the whole body.

## 3. What the build programs actually need from a parsed message

Specs access parsed bodies by name: `transfer.body.destination`,
`internal.body.amount`, `transfer.body.forward_payload`,
`transfer.body.payload_sum_type`, `transfer.body.stonfi_swap_body`
(`specs_decl/jettons.mch`). So the generated Obj must **key exactly** on the
names the specs read — which for clean fields equals the TL-B field names, and
for tail fields are hand-chosen (`forward_payload`, `comment`, `sum_type`, …).

The loader validates the message-type **name** against the registry
(`BuildDriver.cpp:101` checks `message_parsers()`; Python `registry.py:48`
`register_message_type`), and the IR encodes parse statements as
`{"s":"parse","target":"burn","types":["JettonBurn"]}`. The loader does **not**
validate field names — name drift is caught only by the `--parse-dump` A/B gate.
Any codegen must register the generated parser under the same type name and keep
that gate.

## 4. Candidate architectures

### (a) Wrap tlbc output
Keep tlbc; a small generator reads the `.tlb` (or scrapes the generated header)
and emits `parse_<type>` wrappers: `unpack` + converter table + field assembly
for the clean prefix, hand tail via a hook.
- **Effort** medium; **maintenance** two chained codegen passes, header-format
  coupling if scraping.
- **Fatal problem:** the wrapper must *know each field's TL-B type*, which the
  generated header only exposes as a comment. Driving off the `.tlb` means
  writing a mini TL-B parser anyway → this collapses into (b), *plus* you still
  depend on tlbc for the unpack. Worst of both, unless you specifically want
  tlbc's battle-tested bit-fetching. **For our flat schema you don't** (see §5).

### (b) Own minimal TL-B frontend in `mch-codegen`
`mch-codegen` parses the `.tlb` subset we use (untagged `$_` constructors,
explicit `opcode:uint32`, flat fields from a fixed vocab: `uintN`, `intN`,
`VarUInteger 16`, `MsgAddress`, `^Cell`, `Maybe ^Cell`, `bits N`) and emits
complete Value-Obj unpackers directly — no tlbc for these types.
- **Effort** medium. The subset is tiny and regular; recursive-descent parser +
  emitter ≈ 300–500 LOC. `mch-codegen` is already a JSON→C++ tool with the
  CMake custom_command + sha256-guard pattern; a `--msgs` mode fits natively.
- **Maintenance** one tool we own; no vendored-TON fork; no header scraping.
- **Risk** we'd reimplement bit-fetching — *unless* the emitted code **calls the
  existing `PSlice` helpers** (`load_address_py`, `load_coins_py`, `var_uint16`,
  `fetch_maybe_ref`) rather than raw `CellSlice` ops. Then we generate only the
  *orchestration*, the fetch layer is already A/B-proven, and parity risk ≈ 0.
- **Quirk boundary** clean prefix generated, tail hand (first-class hook).
- **ABI fit** the frontend's *output* (a field-descriptor list) is exactly the
  neutral shape option (d) wants; an ABI frontend just feeds the same emitter.

### (c) Runtime interpretive unpacker (no codegen)
Schema → type-descriptor table (build or load time); one generic C++ walker
parses any message per descriptors → named Value fields. Zero per-type code.
- **Against the grain.** The "no hot reload, artifact known at build" decision
  (plan §0.5, overview §4) *removes interpretation's only advantage*. The project
  already chose codegen over interpret for expressions **and** build programs for
  exactly this reason. An interpreter here would be the lone interpretive layer —
  inconsistent, and a per-message perf outlier on the hot path.
- Quirks still can't live in a pure descriptor table → you need descriptor
  "escape" ops or hand fallback anyway. **Rejected as primary.**

### (d) Unification IR (neutral message-shape JSON)
schema source (TL-B now; contract ABI later) → neutral shape JSON
`{fields:[{name,type,value_kind,policy,optional,ref}], quirk_hooks:[…]}` →
downstream = codegen (b) or interpret (c). This is the user's instinct, and it's
**sound as a layering, with one caveat**: the shape must carry the Value-target
**policy** (`Maybe^Cell`→cell|bytes, opcode-drop, Obj|Dict, tail-hook), because
*neither TL-B nor an ABI determines those* — they're consumer decisions. So the
shape is "TL-B type **plus** an mch-value-mapping layer."
- **ABI fit** genuinely strong — a future ABI frontend emits the same shape; the
  emitter is shared. This is the best argument for designing the frontend to emit
  a *shape* rather than emitting C++ directly.
- **Risk** speculative generality if the ABI never materializes (violates the
  repo "no speculative flexibility" rule). Mitigation below.

## 5. Recommendation

**Build (b) with the (d) shape as its in-process intermediate, self-emitting
fetches that call the existing `PSlice` helpers, and drop tlbc from this module.**

Why drop tlbc: for our *flat* schema tlbc buys almost nothing. We don't even use
its `MsgAddress` decode — we re-decode the `Ref<CellSlice>` via `load_address_py`
by hand today. tlbc effectively only fetches `uint32`/`uint64` and hands back
`VarUInteger`/`Maybe^Cell` as sub-slices we re-decode anyway. All of that is
directly replaceable by `cs.fetch_ulong` / `load_coins_py` / `fetch_maybe_ref`,
which are already A/B-proven. Dropping tlbc removes a vendored-tool coupling and
the header-scraping temptation.

Keep the (d) shape **in-process** (a C++ struct inside `mch-codegen`), and only
**promote it to a committed JSON contract if/when a second frontend (contract
ABI) actually lands.** That captures the unification instinct without paying
speculative cost now.

### Staged adoption

- **Stage 0 — proof on cleared ground.** Add `mch-codegen --msgs tlb/mch-msgs.tlb`
  that parses the jetton subset and emits `parse_<type>` for the **fully-clean**
  families (`JettonBurn`, `JettonInternalTransfer`, `JettonMint`) — the ones with
  no tail. Wire as a CMake custom_command mirroring the existing `mch-msgs-tlb`
  one. **Prove:** the generated parsers pass the *existing* `--parse-dump` A/B
  (diff 0 vs `py_msg_dump.py` over the 147-fixture corpus), replacing those hand
  jetton adapters. Zero new parity surface; the ground is already green.
- **Stage 1 — the head/tail boundary.** Generator emits the clean head and a
  `// TAIL: <hook>` call into a hand function for `JettonTransfer`/`JettonNotify`/
  `PTonTransfer`. Register the generated+hooked parser. Gate: same `--parse-dump`
  diff 0.
- **Stage 2 — value-mapping annotations.** Introduce a *minimal* per-field policy
  annotation (in `.tlb` comments or a sidecar) only where TL-B underdetermines:
  `Maybe^Cell`→bytes|cell, Obj|Dict, drop-opcode default. Add only the vocab
  actually needed.
- **Stage 3 — document the boundary for new protocols.** New protocol with clean
  TL-B → write the `.tlb` only, parser generated. Protocol with a quirky tail →
  `.tlb` head + hand tail hook. Fully irreducible → hand parser in the registry.
  **Same registry, same `--parse-dump` gate, for all three.** This is the future
  win: NEW matchers have no Python legacy and are unconstrained by the pytoniq
  quirks that pin the current 5 hand families.
- **Stage 4 — ABI unification (only if it lands).** Promote the in-process shape
  to a committed shape JSON; add the ABI→shape frontend; the emitter is unchanged.
  Not before.

**What proves it:** Stage 0 flipping ≥3 hand jetton adapters to generated with
`--parse-dump` diff 0 over the corpus, and the codegen byte-determinism gate
green.

## 6. Open questions for the user

1. **Drop tlbc from `ton-mch-engine`** (self-emit fetches via `PSlice` helpers)
   vs keep it as the unpack backend? *Recommend drop* — for this flat schema it
   adds almost nothing and forces header-scraping or a fork to get field types.
2. **Where do per-field value-mapping policies live** — `.tlb` comment
   annotations, a sidecar `.json`, or naming convention? *Recommend `.tlb`
   comment annotations* — single source of truth.
3. **Is the contract-ABI frontend real/near enough** to justify designing the
   neutral shape as a *committed contract* now, or keep it in-process until it
   lands? *Recommend in-process now*; promote on arrival.
4. **Should generated parsers replace the hand jetton adapters** (single path,
   −LOC) or coexist? *Recommend replace*, gated by `--parse-dump`.
5. **Scope of the first cut:** jetton subset only (Stage 0–1), or also retrofit
   the clean-head hand families (`PTonTransfer` head, `CoffeeCreateVault`)? The
   effort delta is small once the emitter exists; the decision is how much of the
   existing verified surface to churn.

---

## Addendum — `ton-marker` (existing schema-driven decoder in the repo)

Follow-up: examine `ton-index-worker/ton-marker/` — does it already do schema-
driven message decoding, and does it reuse/overlap/conflict with the option-b
recommendation? **Short answer: it's a working, shipping proof that "schema alone
→ named-field output, zero hand adapter" is real — via a *third* tlbc surface the
study above didn't cover (tlbc's generated `print_skip(JsonPrinter&)`). But it
targets best-effort *labeling*, not DB parity, so it validates feasibility
without displacing option (b) for the parity-grade path. It does sharpen the
new-protocol boundary into an explicit two-tier model.**

### What ton-marker is

A schema-driven message **decoder/labeler** (~740 LOC), built as a **C-API shared
library for Go** (`src/wrapper.{h,cpp}`, consumed by `ton-index-go`) plus a CLI
(`main.cpp`) and a static core (`ton-marker-core`). It is **not on the MCH
classification path** — it answers two questions for the Go indexer:
`decode_opcode(uint32) → constructor name` and
`decode_boc(base64) → JSON string {@type + named fields}`, with recursive
nested-cell decoding. Wired into the root CMakeLists as its own target; uses the
**same vendored `tlbc` target** as `ton-mch-engine`.

### How its decoding works

- **`src/schemes/schemes.tlb`** (2637 lines) — a large opcode catalogue using
  **tagged** constructors (the 32-bit opcode *is* the TL-B constructor tag, e.g.
  `lock_and_claim#3a86f1a0 query_id:uint64 proof:(Maybe ^Cell) = InternalMsgBody0`),
  with full TL-B features (`HashmapE`, `Either`, `Maybe ^Cell`, nested `^Type`
  refs, `Any` catch-all). It is split into `InternalMsgBody0..11` + `ExternalMsgBody`
  + `ForwardPayload` **because tlbc caps at 64 constructors per type** — the split
  is a mechanical workaround, not a semantic grouping.
- **CMake custom_command** runs `tlbc -z schemes.tlb → auto/schemes.{cpp,h}`
  (identical pattern to the mch `mch-msgs-tlb` command).
- **Dispatch (`logic.cpp`)**: `prefetch_ulong(32)` the opcode → linear scan of
  `cons_tag[]` across the 14 parser objects → call tlbc's **generated
  `print_skip(JsonPrinter&, cs)`** to emit named-field JSON. **Zero per-type hand
  adapter.** First-success-wins across parsers; output buffer reset on failure.
- **`special.cpp`** — the hand fallback for schema-*inexpressible* bodies: W5 /
  highload snake-cell **action lists** (tlbc can't count snake links) and
  **opcode-less wallet externals** (v1–v4, highload v1–v3, preprocessed v2). The
  trick: hand-count the snake/ref depth, then instantiate a **parameterized tlbc
  parser** with the computed nat param (`W5MsgBody(out_actions_count,
  extended_actions_count)`, `HighloadV3InternalRequest(n)`, `W4MsgBody(msgs_count)`).
- **Payload cells** (`forward_payload`, `custom_payload`) can't be typed in TL-B,
  so they stay `Cell` and render as `te6c…` BOC strings; `logic.cpp` then
  **re-decodes them by string replacement**, recursively (depth-capped).

### Type/field model

tlbc's **built-in JSON serialization** (`tlb::JsonPrinter`). Values render in
tlbc's own text format — **not** the MCH `Value` model and **not** pytoniq
conventions. No `Amount`/`AccountId`/`Asset` semantics, no `crc32` BOC bytes, no
pytoniq `copy()`/remainder quirks, no conditional-attribute reproduction. Output
is a best-effort **label**, correct in field *names* but not in the
byte/format-exact senses the MCH parity bar requires.

### Reuse / overlap verdict vs options (a)–(d)

- **It exposes a third tlbc exploitation surface.** The study's §1 finding — tlbc
  *record members* collapse `VarUInteger`/`MsgAddress`/`Maybe^Cell` all into
  `Ref<CellSlice>`, losing the semantic type — is about the `unpack`/`Record`
  path. ton-marker sidesteps that entirely by using tlbc's **`print_skip(JsonPrinter)`**,
  which *does* emit names+values (as text) with zero hand code. This is a genuine
  route to "schema → named fields" and it is already in production here.
- **New sub-option (a2): tlbc JsonPrinter → JSON → parse into `Value`.** Proven,
  schema-only, zero per-type code. **Disqualified for the parity-grade path** by
  four things: (i) value formatting is tlbc's, not pytoniq (addresses, amounts,
  bytes render differently → DB-visible divergence); (ii) **no pytoniq quirk
  parity** — exactly the `forward_payload` crc32 bytes / `copy()` ref-reset /
  conditional `sum_type` fields the plan §11 C1-correction flags as load-bearing
  and DB-visible *today*; (iii) double serialize + JSON reparse on the hot path;
  (iv) the 64-cons split + linear opcode scan. Bridging (i)+(ii) means
  hand-patching precisely the fields that matter — which erodes the zero-code
  advantage and lands you back at option (b)'s shared, A/B'd helpers.
- **No conflict with "drop tlbc."** ton-marker keeps tlbc and uses its JsonPrinter;
  the study recommends mch self-emit and drop tlbc. Different modules, different
  goals — not contradictory. Data point: *if* mch chose to keep tlbc, JsonPrinter
  is a lower-effort route than Record-scraping for the clean cases.
- **`special.cpp` is a reusable pattern for the head/tail boundary (Stage 1).**
  Feeding a parameterized tlbc parser a hand-computed nat param is a concrete,
  shipping instance of "generated body + hand-computed shape input" — the same
  shape as MCH's "clean head + hand tail." Reusable *conceptually* even if mch
  drops tlbc.
- **Schema duplication is a latent risk.** Two TL-B schemas now coexist —
  `mch-msgs.tlb` (untagged + explicit `opcode` field, soft-parse-lenient,
  pytoniq-faithful) and `ton-marker/schemes.tlb` (tagged, best-effort) — modeling
  **overlapping opcodes** (jettons, dedust, …) with **different conventions and
  different fidelity**. They are not interchangeable (soft-parse leniency + quirk
  parity vs tagged best-effort). If mch expands its own `.tlb`, the two drift
  independently. The (d) unification instinct extends here in principle, but the
  semantic gap (parity vs label) means a shared *schema* would still need
  per-consumer value-mapping layers — reinforcing §4(d)'s conclusion that the
  neutral shape must carry consumer policy, not just TL-B types.

### Does it change the recommendation or the staging?

**Recommendation unchanged.** Option (b) — own minimal frontend, self-emit calling
the A/B'd `PSlice` helpers — remains the right **parity-grade** path, because
JsonPrinter output cannot meet the pytoniq/DB parity bar without a value-format
bridge plus quirk hand-patching that negates its zero-code appeal.

**One structural addition — make the tier boundary explicit.** ton-marker is the
reference implementation for a *best-effort, no-parity* tier that the study only
implied. Recast Stage 3's new-protocol boundary as **two tiers**:

- **T1 parity-grade** (feeds build programs / DB-visible fields): option (b)
  generated clean head + hand tail, `--parse-dump` diff-0 gated. Unchanged S0–S2.
- **T2 best-effort labels** (a future coarse use — surfacing an opcode name or
  rough fields with no parity claim): tlbc + JsonPrinter, ton-marker-style —
  potentially by **literally reusing `ton-marker-core`** (its C-API already
  exists) or sharing `schemes.tlb`, rather than growing the MCH parser layer.

**Minor staging note:** S0's generated clean-jetton parsers could be cross-checked
against ton-marker's JSON for the same opcodes as a *free second oracle*, though
`py_msg_dump.py` remains the authoritative parity gate.

**New open question for the user (6):** are the MCH parse layer and ton-marker
destined to stay two separate decoders (parity vs label), or should their schema
sources converge under the (d) shape? Convergence is attractive but blocked by the
tagged-vs-untagged + parity-vs-best-effort semantic gap; recommend **keep separate
now**, revisit only if a T2 use for MCH actually materializes.

---

## Option (e) — share ton-marker's decoder layer as one decoding truth

Further context from the user: **ton-marker is already the production message
decoder for the Go API (`ton-index-go`)** — message decoding *already flows
through it in production*, and it decodes **forward payloads** too, i.e. exactly
the area where the MCH jetton adapters carry the pytoniq quirks. So evaluate
sharing marker's decoder layer as a first-class option: one decoding truth for
both the Go API and the MCH engine, instead of (or alongside) the mch-codegen
TL-B frontend.

### (1) How marker's decoders are defined

**Schema/table-generated, not hand-written per opcode** (except the wallet
fallback). `schemes.tlb` is a large, production-maintained catalogue of **tagged**
constructors (opcode = 32-bit tag) covering dozens of protocols. `tlbc` generates
the decoders; `logic.cpp` dispatches by opcode and calls the generated
`print_skip(JsonPrinter&)`. The only hand code is `special.cpp` (wallet externals
+ snake action lists) — the schema-inexpressible tail, same boundary MCH has.

### (2) Output model

A **JSON string** via tlbc's `JsonPrinter`. It has named fields (mappable in
principle to a `Value` Obj), **but the value formatting is raw per-field TL-B, not
pytoniq**. Confirmed from `tlblib.hpp` (`JsonPrinter` only exposes
`fetch_bits_field` / `fetch_int_field` / `fetch_uint256_field` / `out_integer` —
no address/coins helpers): a `MsgAddress` renders as its constructor fields
(`addr_std` → `{anycast, workchain_id, address:<bits256 hex>}`), `Coins`/`Grams`
render as `{len, value}` (or a VarUInteger sub-object), and cells render as td
`std_boc_serialize` `te6c…` strings. None of these match the MCH `Value` forms
(`AccountId.as_str()` = `"wc:HEX"` uppercase, `Amount`/int, `crc32` pytoniq-order
BOC bytes).

### (3) Forward payloads / nested payloads vs the PSlice quirk semantics

Marker **does** type many forward payloads that MCH hand-reads — `schemes.tlb`
has a `ForwardPayload` type with the exact opcodes: `stonfi_swap#25938561`,
`encrypted_text_comment#2167da4b`, plus dedust / coffee / evaa / tegro / moon /
bidask variants. **But the mechanism is fundamentally different**: marker emits
the payload cell as a `te6c…` td-BOC string and then **re-decodes nested payloads
by recursive string-replacement of those substrings in the JSON** (`logic.cpp`
`replace_boc_cells_recursive`, depth-capped). It does **not** reproduce pytoniq's
`Slice.copy()` ref-reset, the "remainder bits as a cell" path, the inline-vs-ref
`forward_payload` split, or the conditional `sum_type` attribute — the precise
quirks the `PSlice` layer exists to replicate (MsgParse.cpp header catalogue).
For a payload where those quirks change the bytes or the field set, marker's
output diverges.

### (4) Could MCH share marker's decoder layer? Parity risk.

**Verdict: not as the MCH decoder for parity-grade fields; viable only as a
schema *source*, and conditionally for a best-effort tier.**

- **Byte/format gap lands on the DB-visible fields.** To turn marker JSON into a
  parity `Value` you must reformat every address (`{workchain_id,address}` →
  `"wc:HEX"`, and match pytoniq's `addr_none`/`extern:<len>:<value>`/anycast-
  consumed-not-applied quirks), every coins field (`{len,value}` → int), **and
  re-serialize every payload cell to pytoniq `crc32` BOC bytes** (marker gives
  td-order `te6c` strings — wrong order *and* wrong crc flag for the DB-visible
  `forward_payload`/`custom_payload`). That is hand-fixing exactly the fields that
  matter — the same wall option (a2) hit, now behind a heavier dependency.
- **A/B is against the Python pytoniq oracle, not against marker.** Even a *new*
  protocol matcher is verified by byte-identical diff vs a Python twin that parses
  with pytoniq. Address/coins fields *can* be normalized to agree at the `Value`
  level on both sides; the irreducible gap is **Cell-derived byte fields**, where
  pytoniq order ≠ td order. So marker-sourced parsing can only A/B-clean for
  protocols that don't store raw payload BOC bytes (or that canonicalize them via
  one writer on both sides).
- **"One decoding truth" is already two.** Because the Go API renders forward
  payloads via marker (td/raw format) and MCH renders them via pytoniq, the two
  truths **already diverge in production**. Unifying on marker means accepting a
  reindex/format delta on MCH's existing DB fields — a large, out-of-scope
  decision, not a free consolidation.
- **Model + coupling mismatch.** Marker is tagged-constructor, best-effort,
  substring-nested-decode, JSON-text, 64-cons-split, and **shared cross-repo with
  Go**. MCH is untagged soft-parse-lenient, slot-typed `Value`, two-phase lookups.
  Adopting marker's runtime couples the MCH hot path to a JSON reparse and to a
  schema owned by another consumer.

**The real, cheaper win inside (e): harvest `schemes.tlb` as a schema *source*,
not the runtime.** It is a curated, production-maintained opcode + field-shape
catalogue — precisely the input the option-(b) frontend / the (d) shape wants.
MCH can lift the relevant constructors into its own `.tlb`/shape (untagged +
explicit-opcode form) and generate parity-grade parsers from them, getting "one
schema truth" **without** importing marker's value-format, byte-order, coupling,
or quirk-fidelity problems. This satisfies the user's consolidation instinct at
the schema layer while keeping the runtime parity-safe.

### Effect on the recommendation and staging

- **Recommendation unchanged** for the runtime: option (b), self-emitting parity
  parsers. **Do not** route MCH message parsing through ton-marker's decoder.
- **Add to the two-tier model:** for **T2 best-effort labels**, sharing marker
  (or literally calling `ton-marker-core`) is the natural choice and needs no new
  MCH code — that tier has no pytoniq-parity claim.
- **New staging item (S2.5 / a Phase-C input step):** when adding a family, first
  check `schemes.tlb` for its constructors and **harvest the field shape** into
  the MCH schema rather than re-deriving from the Python parser. Cross-check the
  generated MCH parser's field *names* against marker's JSON for the same opcode
  as a free second oracle (`py_msg_dump.py` stays the byte-parity gate).
- **New open question (7):** is "one decoding truth for Go API + MCH" a hard
  product requirement (→ forces confronting the pytoniq-vs-td byte delta on
  existing DB rows, a reindex decision), or is **schema-source sharing** (harvest
  `schemes.tlb`, keep two runtimes) sufficient? Recommend the latter unless the
  product genuinely needs identical bytes across both surfaces.
