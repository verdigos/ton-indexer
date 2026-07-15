# Tolk-ABI → C++ port — design study (2026-07-15)

Question: what would a complete C++ port of `S:\Dev\tolk-abi-to-typescript` codegen look
like, as the eventual replacement for TLB-based message parsing in the indexer?
Sources: two exploration reports (tolk tool map ×3 parts; ton-index-worker TLB/parse
surface map) — facts referenced by file:line come from those.

## 1. What the TS tool actually is (relevant essentials)

- INPUT: Tolk-compiler-emitted **ABI JSON** (`ContractABI`, abi.ts:109) — all types interned
  in a flat `unique_types: Ty[]`, referenced everywhere by integer index. ~30 type kinds
  (abi-types.ts:12-45). Generics arrive PRE-MONOMORPHIZED (`struct_instantiations` +
  `monomorphic_fields_ty_idx`); client never reconstructs generic layouts.
- TWO consumers of the same rules, implemented TWICE:
  (a) static codegen → `.gen.ts` wrapper (template + emitters), (b) **dynamic interpreter**
  (`dynamicPack`/`dynamicUnpack` — big switch over `Ty.kind` walking cells at runtime).
- Shared kernel `types-kernel.ts` (326 LOC): SymTable index resolution, monomorph
  substitution (`structFieldsOf` with `uLabelTyIdx` back-ref for union `$` labels),
  `createLabelsForUnion`, `calcWidthOnStack`. **The load-bearing module.**
- Prefixes: per-struct `prefix{num,len}` — ANY bit length; parse CHECKS the prefix
  (throw on mismatch). Unions = prefix trees (implicit prefixes eat bits, explicit ones
  are re-read by the variant's own loader; `T|void` = empty-slice check; 2-variant
  implicit = Either bool).
- Errors: always THROW, never null. No exotic cells, no bounce handling, eager parse only.
- Runtime = @ton/core Slice/Builder/Dictionary/TupleReader; all helpers inlined per
  generated file, feature-gated.
- ~4,000 LOC src; conformance = 14 Jest specs over 26 .tolk fixtures compiled at test time,
  every getter asserted static==dynamic. README: this repo IS the reference implementation;
  porters should copy the algorithms and the tests.

## 2. What it replaces on our side

From the surface map of ton-index-worker:
- REPLACEABLE: the mch message-parse layer — `mch-msgs.schema` (145 lines, bespoke DSL) +
  `msgs_gen.py` → committed `MsgParsersGenerated.cpp` (14 registered + 9 host-embedded
  types), plus the 2 residual tlbc unpacks (`ParseJettons.cpp:53,167`, `mchmsg::gen`).
- COEXISTS, NOT REPLACED: `block::gen` (TON block.tlb — structural, untouchable);
  `tokens::gen` (tagged TEP-74/62 in tondb-scanner/EventProcessor — separate subsystem,
  could migrate LATER once the port is proven); hand parsers for irreducible cases
  (dns, layerzero, soft-partial NFT, forward-payload tail) stay hand regardless of schema
  technology.
- CONTRACT that must survive: parsers are `MsgParserFn = td::Result<Value>(const
  td::Ref<vm::Cell>&)` registered in `message_parsers()` (MsgParse.cpp:49-67); soft-parse
  error policy at the registry; outputs are `mch::Value` Obj consumed by specs/hosts.

## 3. Port shape: INTERPRETER FIRST, codegen optional later

The TS tool implements the serialization rules twice (emitters + dynamic switch). A port
does NOT need both. Recommended: port the **dynamic interpreter** as the primary artifact.

Why interpreter-first for an indexer (vs our usual codegen habit):
1. New protocol = drop an ABI JSON + registry config, **no rebuild** — operationally
   valuable for an indexer in a way it isn't for a wallet SDK.
2. Perf is a non-issue at our scale: whole classify = ~3 ms/trace; message parse is a
   fraction. Tree-walk over an interned type table is cheap; type table can be
   pre-resolved at load (see §5).
3. Matches our proven artifact discipline: committed JSON + fail-closed loader + twins/
   vectors (exactly the mch_ir.json model).
4. Codegen (emitting C++ that calls the same leaf functions) can be added later as a pure
   optimization with the interpreter as its A/B twin — same one-semantics pattern as
   ExprRuntime cores.

What we do NOT port at all:
- Contract class / sendXXX / provider / deploy plumbing (client-SDK concerns).
- TS template/OutBuf machinery (only needed for the codegen mode, and a C++ codegen mode
  would reuse our existing generator discipline instead).
- Get-method/stack path (TupleReader/StackReader/type-id unions) — indexer parses message
  BODIES; getter decoding is a separate later feature (could serve interface detection in
  tondb-scanner eventually; the ABI carries get_methods when we want it).
- PACK side initially: indexer consumes; it does not construct messages. Pack = phase 4
  (useful for test-vector generation and emulation, not for classification).

## 4. C++ architecture (concrete)

New static lib `ton-abi` (location: ton-index-worker/ton-abi/ or inside tondb-scanner;
mch links it — keep it mch-independent so tondb-scanner can use it later):

```
ton-abi/
  AbiModel.h/.cpp      — ContractABI + Ty POD structs mirroring abi.ts/abi-types.ts
  AbiLoader.h/.cpp     — JSON → AbiModel; FAIL-CLOSED (unknown kind/index OOB/missing
                         field = load error, mirrors mch IrLoader discipline);
                         bigint-as-string decode to td::RefInt256
  AbiKernel.h/.cpp     — port of types-kernel.ts: SymTable (name→decl, idx→Ty),
                         structFieldsOf w/ monomorph substitution + client_ty_idx swap,
                         aliasTargetOf, createLabelsForUnion, (calcWidthOnStack only
                         when the stack path arrives)
  AbiUnpack.h/.cpp     — the dynamic interpreter: unpack(ctx, ty_idx, vm::CellSlice&)
                         → td::Result<AbiValue>; big switch over TyKind, one case per
                         abi-types kind; prefix-tree union dispatch ported exactly
                         (implicit-eats / explicit-rereads / void-empty-slice / Either)
  AbiValue.h           — output value model (see §6)
  AbiVectors.cpp       — conformance runner (committed vectors, see §7)
  (later) AbiPack.cpp  — builder side
  (later) AbiStack.cpp — get-method tuple path
```

Leaf ops → TON-core vm API (from the surface map, all already in use):

| Ty kind | C++ leaf |
|---|---|
| intN/uintN | `cs.fetch_int256(n, signed)` → RefInt256 |
| varintN/varuintN | 4/5-bit len nibble + bytes (log2(n) width) — small helper, semantics copied from @ton/core loadVarIntBig |
| coins | = varuint16 (`block::tlb::t_VarUInteger_16.as_integer` or own helper) |
| bool | `cs.fetch_ulong(1)` |
| cell | `cs.fetch_ref()` |
| address / addressOpt / addressExt / addressAny | anycast-aware MsgAddress reader over `cs` (NOT load_address_py — see §6 conventions) |
| bitsN | `cs.fetch_bits(n)` → own bits value |
| string | snake-in-ref reader (have load_snake_bytes precedent) |
| remaining | rest of slice+refs → Cell (CellBuilder) |
| cellOf<T> | fetch_ref → recurse in fresh slice |
| mapKV | `vm::Dictionary(root, key_width)` walk; keys intN/uintN/address only (tool's own restriction) |
| tensor/shapedTuple | inline field-by-field recurse |
| nullable | 1-bit maybe + recurse |
| arrayOf/lispListOf | ref-chain walkers (port template helpers) |
| EnumRef | recurse into encoded_as_ty_idx, value = int |
| StructRef/AliasRef | kernel-resolved fields, recurse |
| union | prefix-tree dispatch → variant value + `$` label |
| int/slice/builder/callable/unknown | load = error (same as tool: CantGeneratePackUnpack class) |

Registry bridge (mch side, thin): `abi_message_parsers(vector<AbiFile>)` — for each ABI's
`incoming_messages[].body_ty_idx`, derive (name, opcode-from-prefix, thunk closing over
(AbiModel, ty_idx) → unpack → Value). Rows merge into `message_parsers()` exactly like
`generated_message_parsers()` today. NOTE our current parsers SKIP the 32-bit opcode
unchecked (dispatch already matched it); tolk unpack CHECKS the prefix — bridge either
re-feeds the full body (prefix check = free consistency assert) or strips per current
convention; recommend CHECK (stricter, and dispatch/parse can't drift).

## 5. Interpreter perf note (so nobody redesigns for speed later)

At load: resolve the interned model once — flatten each StructRef's monomorphized field
list to a vector<(field_name, ty_idx)>, pre-build union prefix tables (sorted by len then
value), pre-decode all bigint strings. After that, per-parse cost is pointer-chasing a
small resolved table. If profiling EVER shows this hot, the codegen escape hatch is
mechanical: walk the same resolved model and emit C++ calling the SAME leaf helpers
(one-semantics, interpreter as twin) — our W2/mch-codegen discipline applies verbatim.

## 6. Two decisions that shape everything (user calls)

**D1 — Value conventions: tolk-faithful vs pytoniq-parity.**
Our existing 23 generated/schema messages emit pytoniq conventions (addr_none→Null,
extern→"extern:len:val" Str, RefInt256 amounts, account "wc:HEX" strings — PSlice layer).
Tolk/@ton/core conventions differ (addressAny 'none' sentinel, throwing loadAddress,
Address objects). Options:
  (a) Port stays TOLK-FAITHFUL; NEW protocols onboarded via ABI use tolk conventions;
      legacy 23 stay on mch-msgs.schema path forever (or until individually migrated with
      spec-side adjustments). Two parse layers coexist by design. RECOMMENDED — clean
      reference-implementation port, conformance vs the TS tool possible, no parity
      shimming.
  (b) Port emits pytoniq conventions (configurable leaf layer) so legacy messages can
      migrate byte-identically. Pollutes the port with our quirk shims, kills TS-tool
      conformance, saves ~145 schema lines. NOT recommended.
With (a): mch-msgs.schema path is FROZEN (no new entries), ages out; ABI path is the
onboarding default once landed.

**D2 — Where do ABI JSONs come from for third-party protocols?**
We index contracts we didn't write; there is no upstream tolk source for stonfi/dedust/etc.
The ABI JSON is machine-oriented ("hard to read", interned indices) — hand-authoring it is
worse than our schema DSL. The right authoring flow: write a minimal **.tolk declaration
file** per protocol (struct decls + opcode prefixes only, no code) and run the tolk
compiler (tolk-js, offline, like msgs_gen.py runs today) → ABI JSON committed. Authoring
UX ≈ today's schema (arguably better: standard language, type-checked), artifact =
standard ABI. When protocols start SHIPPING official tolk ABIs, we consume them directly —
that is the strategic payoff of "moving from tlb to this".

## 7. Conformance (the port's gate set)

README's instruction: copy the algorithms AND the tests. TS tests run on sandbox TVM —
not portable. Portable equivalent, matching our vectors discipline:
1. Node-side generator script (dev-only, uses the TS package itself): for each of the 26
   fixture ABIs + a set of sample bodies, run `unpackFromSliceDynamic`, dump (ABI, body
   BoC b64, expected value JSON) triplets → `abi_vectors.json` COMMITTED.
2. C++ ctest: AbiLoader + AbiUnpack over the vectors, compare canonical-JSON dumps.
   Error cases too (prefix mismatch, union no-match, truncation per field — the tool
   throws; we expect td::Status error).
3. Round-trip vectors once pack lands.
This gives the port the same evidence quality as parse-dump/msg-vectors gave W2/W3.

## 8. Effort + phases (honest)

| Phase | Content | Est. LOC | Notes |
|---|---|---|---|
| P1 | AbiModel + fail-closed AbiLoader + AbiKernel + AbiUnpack (no pack/stack) + vectors harness + TS-side vector generator | ~1,200-1,500 C++ + ~200 TS script | the port core; 2-3 waves with gates |
| P2 | Registry bridge into mch `message_parsers()` + first NEW protocol onboarded via .tolk-decl → ABI | ~150 | proves the flow end-to-end |
| P3 | tondb-scanner adoption path (tokens::gen replacement) — optional, separate decision | ~200 bridge | tagged TEP-74/62 fits the prefix model naturally |
| P4 | Pack side + get-method/stack path + codegen mode | ~1,000 | only on demonstrated need |

Risks: (i) tolk ABI schema evolves with the compiler (tracks v1.4.1 today; abi_schema_version
field exists — loader pins supported version, fail-closed); (ii) varint/address leaf
semantics must be copied from @ton/core exactly (vectors catch drift); (iii) generic-union
reshape gap exists upstream too ("never occurs in practice" per README) — inherit the
limitation, document it.

## 9. Runtime layer spec (L1) — user-refined direction 2026-07-15

User direction: codegen IS the eventual goal (rewrite all parsers to tolk ABI); interpreter
acceptable start; the load-bearing question = the BODY-PARSER RUNTIME API. Wider than mch.

Layering (one-semantics rule: interpreter and generated code call the SAME L1 leaves —
ExprRuntime pattern):

```
L0  TON-core vm                  — have
L1  tolk-runtime leaf lib        — build ~600-800 LOC (THE deliverable)
L2a interpreter AbiUnpack        — first, stays forever as codegen's A/B twin
L2b codegen emitter → C++ structs calling L1 — the goal (offline generator,
    msgs_gen/mch-codegen discipline: committed TU + repro guard)
L3  consumers: mch Value bridge / tondb-scanner typed records / other tools
```

L1 inventory — HAVE from L0: vm::CellSlice fetch_* family, fetch_ref/maybe_ref,
CellBuilder, t_VarUInteger_16 (coins), vm::Dictionary, RefInt256, abort-safe open
patterns, td::Result model, std_boc_serialize.

L1 SCRATCH (each small, ALL vectors-gated, semantics copied from out-template.ts +
@ton/core — NOT from our pytoniq-parity PSlice layer; keep the two universes separate):
1. prefix ops: load_and_check_prefix (any bit len), lookup_prefix peek, union prefix-tree
   dispatch (implicit-EATS / explicit-REREADS / T|void empty-slice / Either fast path) ~80
2. address family ×4: load_address (addr_std only, no anycast — @ton/core parity, NOT
   load_address_py), load_maybe_address, load_external_address, load_address_any ~120
3. general varintN/varuintN (log2(n)-bit len + bytes) ~30
4. load_string_ref_tail snake (@ton/core parity; do NOT reuse pytoniq load_snake_bytes —
   write fresh vs vectors) ~30
5. bitsN holder ~20;  6. remaining→Cell snapshot ~20
7. arrayOf loader per COMPILER wire (chunked refs, 8-bit len); store matches compiler wire
   → KNOWN divergence vs TS store (TS: 1 elem=1 ref, out-template.ts:141) — document in
   conformance known-divergence list ~60
8. lispListOf walker ~30;  9. nullable 1-bit maybe (wide-nullable = stack path, later)
10. dict adapter: vm::Dictionary walk → per-entry recurse (keys intN/uintN/address) ~60
11. error chain with field-path locality via Status::move_as_error_prefix

Non-leaf scratch: AbiModel + fail-closed loader ~350; AbiKernel (types-kernel.ts port:
SymTable, structFieldsOf monomorph+client_ty_idx swap+uLabelTyIdx, createLabelsForUnion)
~300 — copy algorithms verbatim; load-time resolution pass (flatten indices → field
vectors + sorted prefix tables).

Codegen target shape (L2b): per ABI struct → C++ struct { static PREFIX; typed fields;
static td::Result<T> from_slice(vm::CellSlice&); td::Status store(CellBuilder&) const; }.
Types: intN/uintN/varint/coins→RefInt256; bool→bool; cell→Ref<Cell>; address→typed addr
value; nullable→td::optional; union→std::variant + prefix-tree from_slice; enum→enum
class; cellOf<T>→wrapper; generic instantiations→concrete mangled structs
(BasicStorage_int64) per monomorphization entry. tondb-scanner consumes these like
tokens::gen Records today.

Conformance: node script drives the TS package over its 26 fixtures → committed
(ABI, body-BoC, expected-value-JSON) triplets → C++ ctest runs BOTH interpreter and
generated structs. Estimates: L1 600-800, loader+kernel ~650, interpreter ~500, emitter
800-1,200, mch bridge ~150.

## 10. Bottom line

Port = **~1.5k LOC interpreter-first C++ library** (AbiLoader + kernel + dynamic unpack)
with committed conformance vectors generated by the TS reference tool; a thin registry
bridge makes ABI-defined messages first-class `message_parsers()` rows. No codegen needed
initially — the ABI JSON *is* the artifact, matching our IR discipline. Authoring for
third-party protocols = minimal .tolk declaration files compiled offline to ABI. Legacy
mch-msgs.schema freezes; block::gen untouched; tokens::gen migrates later if wanted.
Decisions needed from user: D1 (tolk-faithful conventions — recommended) and D2 flow
sign-off, plus where the lib lives (ton-abi/ standalone recommended).
