# ton-abi module — implementation plan (self-contained; PROGRAM COMPLETE through W12)

> **STATUS (2026-07-24): ALL WAVES W0-W12 EXECUTED + COMMITTED on branch `ton-abi`.**
> This doc = plan AS WRITTEN + wave log AS BUILT. For CURRENT state read `## 6. Wave log`
> TOP-DOWN (newest first) — later waves AMEND earlier sections: W11.5 retired the
> interpreter (pure codegen; AbiValue = test-gate + bridge interchange only), W12
> reconciled §0.16 (see ton-mch-engine/W12-BRIDGE-DESIGN.md + T122-CLAIMREWARD-DESIGN.md
> — those two are the bridge's as-built truth, §0.16 here is the pre-W11.5 original).
> §0.16 bare-name amendment + §0.12 array policy are inline-amended and current.
>
> **FRESH-SESSION ENTRY MAP**: this file (plan+log) → module docs at
> ton-index-worker/ton-abi/ (W11-SHAPE-SPEC.md = emitter contract,
> testdata/{VALUE_SCHEMA.md,ERROR_MAPPING.md}) → bridge docs at
> ton-mch-engine/{W12-BRIDGE-DESIGN.md,T122-CLAIMREWARD-DESIGN.md}. Gates:
> ton-abi ctest 2/2 (build dir cmake-build-debug-visual-studio/ton-abi, run ctest from
> that subdir), ctest -R mch 8/8 (…/ton-mch-engine subdir), mch twin gate set =
> docs/mch-cpp-plan.md §1 (**baseline = 148 fixtures / 199 actions** — fixture swap
> RESOLVED 2026-07-24 keep-both: 02kEPzkQ restored + i-gQwY3n committed;
> classify/actions/match re-verified diff-0 at 148). OPEN ITEMS: TON-payout YAML
> case (fixture fetch needs n2+VPN), merge decision, further protocol migration
> (§0.6 legacy ages out per-protocol).
>
> Companion design study (historical, partially superseded):
> docs/tolk-abi-cpp-port-design.md. Review history §R/§R2-R6 (5 rounds, all resolved).

## A. Background & motivation

The TON indexer (S:\Dev\ton-indexer-clear) classifies blockchain traces. Its C++ side
(ton-index-worker) parses protocol message bodies today via THREE mechanisms:
1. `tlbc`-generated unpack code from hand-written TLB files (TON-core codegen tool;
   project schemas: `ton-mch-engine/tlb/mch-msgs.tlb` — nearly dead, 2 call sites;
   `tondb-scanner/src/tlb/tokens.tlb` — tagged TEP-74/62, alive).
2. A bespoke schema DSL (`ton-mch-engine/mch-msgs.schema`, 145 lines) compiled by an
   offline Python generator (`msgs_gen.py`) into committed cursor-walk C++
   (`MsgParsersGenerated.cpp`, 22 message types) over a pytoniq-parity helper layer
   (`PSlice`).
3. Hand-written parsers for irregular cases (dns, layerzero, soft-partial NFT bodies).

Strategic direction (user, 2026-07-15): migrate message-schema definition to the **Tolk
ABI** — the JSON contract-interface format emitted by the Tolk compiler (TON's new
contract language). Rationale: standard format, third parties will ship official ABIs,
kills bespoke schema maintenance. `S:\Dev\tolk-abi-to-typescript` (TON Core, MIT, ~4k LOC
TS) is the **canonical reference implementation** of Tolk ABI (its README instructs
porters to copy its algorithms and tests). This plan builds `ton-abi`: a standalone C++
module porting that library's message-body (de)serialization, with its test suite
preserved.

NOT in scope of this plan: TON block-structure parsing (`block::gen` from TON-core
block.tlb — untouchable), the mch classification engine itself, tondb-scanner migration
(future), TVM get-method support (deferred, §5).

## B. Reference-tool facts (from full exploration of S:\Dev\tolk-abi-to-typescript)

- INPUT: `ContractABI` JSON (src/abi.ts:109) emitted by Tolk compiler v1.4.1. All types
  interned in flat `unique_types: Ty[]`, referenced by integer index everywhere. ~30 Ty
  kinds (src/abi-types.ts:12-45): intN/uintN/varintN/coins/bool/cell/address(4 variants)/
  bitsN/string/remaining/nullable/cellOf/arrayOf/lispListOf/tensor/shapedTuple/mapKV/
  EnumRef/StructRef/AliasRef/union/genericT + non-serializable (int/slice/builder/
  callable/unknown/void/nullLiteral). Bigints JSON-encoded as strings. Struct opcodes =
  `prefix{prefix_num, prefix_len}` — ANY bit length, CHECKED on parse (throw on mismatch).
- GENERICS arrive pre-monomorphized: `struct_instantiations`/`alias_instantiations` carry
  `monomorphic_fields_ty_idx`; kernel substitutes at use-site (`structFieldsOf`,
  types-kernel.ts:69-95, with `uLabelTyIdx` back-ref so union `$` labels stay generic).
  `@abi.clientType` (`client_ty_idx`): a field's cell/client type ≠ stack type; cell side
  matters for us.
- UNIONS: prefix trees per variant (`prefix_num/len/is_prefix_implicit`); implicit
  prefixes EAT their bits, explicit are RE-READ by the variant's own loader; `T|void` =
  empty-slice check; 2-variant implicit = Either bool fast path
  (src/emit-pack-unpack.ts:107-145).
- TWO consumers of the same rules implemented twice: static codegen (generated .gen.ts
  wrappers) + dynamic interpreter (dynamicPack/dynamicUnpack switch). Shared kernel =
  types-kernel.ts (326 LOC) — THE load-bearing module.
- Errors: always throw, field-path locality. No exotic cells, no bounce handling, eager
  parse. Known upstream gaps we inherit: generic-union reshape after monomorphization
  ("never occurs in practice"), map keys restricted to intN/uintN/address, custom
  serializers not on generics. arrayOf: TS STORE deliberately diverges from compiler wire
  (1 elem = 1 ref vs chunked; out-template.ts:141).
- Runtime deps of emitted TS: @ton/core only; helpers inlined per file. Custom-serializer
  seam: `registerCustomPackUnpack(name, pack, unpack)`.

## C. Our-side facts (ton-index-worker survey)

- C++ cell API available: `vm::CellSlice` (fetch_ulong/long/int256/bits, fetch_ref/
  maybe_ref, have/size_refs), `vm::CellBuilder`, `vm::Dictionary`, `td::RefInt256`,
  `block::tlb::t_VarUInteger_16` (coins), `vm::load_cell_slice_special` (abort-safe
  wrappers exist as pattern), `td::Result`/`td::Status`, `vm::std_boc_serialize`.
- `PSlice` (ton-mch-engine/src/parse/) = pytoniq-parity leaf layer (addr_none→Null,
  extern→"extern:len:val" strings, etc.). AUDITED: nothing migrates into ton-abi — every
  helper encodes pytoniq quirks incompatible with @ton/core semantics. Patterns reuse
  only (abort-safe open, fail-closed loader discipline).
- Precedents this plan reuses: committed-generated-TU + `--check` byte-repro guard
  (msgs_gen.py), build-time C++ codegen exe (tlbc, mch-codegen), interpreter+codegen twin
  gating (mch IR engine vs generated builds), data-driven ctest vectors.
- Build env: Windows, vcpkg toolchain, PowerShell builds, `cmake --build` -j4 cap; ctest
  present (mch registers 7 tests).

## D. Design summary (why this shape)

Layering (one-semantics rule — interpreter and generated code call the SAME leaves):
```
L0 TON-core vm (have) → L1 leaf runtime (~600-800 LOC, THE correctness-critical work)
→ L2a interpreter (AbiUnpack/AbiPack walkers) → L2b ton-abi-gen emitter (C++ exe)
→ L3 consumers (mch bridge; later tondb-scanner)
```
- Interpreter first (drop-an-ABI onboarding, no rebuild), codegen is the goal (typed
  structs like tokens::gen Records). Interpreter stays forever as the emitter's A/B twin.
- Effort basis (evaluated): ABI parsing is cheap (~500-600 mechanical LOC) → no IR middle
  layer needed (option A: C++ consumes official ABI JSON directly). Pure C++ emitter
  REUSES model+kernel (written once); a Python emitter would secretly reimplement both.
  Node appears only as offline truth-oracle (vector generation via the TS package itself —
  reimplementing it would be circular).

## 0. LOCKED DECISIONS (user, 2026-07-15)

1. **Scope = message parsing ONLY** — cell serialization unpack+pack. OUT (additive
   later, §5): get-methods, TVM stack machinery (calcWidthOnStack, StackReader, stack
   type-ids, wide-nullable-on-stack), contract class/sends, debug-print, emulator.
2. **Pure C++ self-contained**: runtime + interpreter + emitter all C++; `ton-abi-gen`
   exe reuses AbiModel/AbiLoader/AbiKernel. No Python anywhere. Node = dev-only vector
   oracle.
3. **Pack IN scope**: the reference tests assert store→golden-hex round-trips; pack
   shares leaves; enables future construction.
4. **Option A** — no IR middle layer; C++ loads official ABI JSON directly.
5. **NO mch dependency**: ton-abi includes nothing from ton-mch-engine (no mch::Value,
   no PSlice). Bridge lives in ton-mch-engine and links ton-abi, never reverse.
6. **Tolk-faithful value conventions** (@ton/core parity, NOT pytoniq). Legacy
   mch-msgs.schema path freezes and ages out.
7. **doctest** vendored single header = test harness (revisit only if W0 hits a wall).
8. **Custom-serializer registry seam in scope** (10 reference test types need it); not
   for generics (upstream limitation inherited).
9. Emitter output = committed generated .h + .cpp pair + `ton-abi-gen --check` repro
   guard; NOT build-tree generation.
10. Errors: td::Result/td::Status with field-path prefixes. Reference exact-message
    asserts port as trigger+category with re-specified C++ messages (mapping committed).
11. Third-party protocols (no tolk source exists): author minimal .tolk DECLARATION
    files (structs + opcodes only), compile offline via tolk compiler → ABI JSON
    committed. When protocols ship official ABIs, consume directly.
12. arrayOf wire (REVISED after plan review): PACK = TS-reference parity (1 elem/ref) so
    the Node oracle stays authoritative for ALL pack vectors; UNPACK accepts BOTH the
    compiler chunked format AND 1-elem/ref. Chunked-unpack coverage = hand-built cells
    per the documented compiler format (best-effort) + deferred item: compiler-executed
    vectors via sandbox (store-on-TVM → committed cells) if arrays appear in real
    protocol messages.
12b. KNOWN-DIVERGENCE #2 (decided at W4): ANYCAST internal addresses = REJECT (lead
    decision on executor-surfaced conflict). @ton/core _loadInternalAddress PROCESSES
    anycast (rewrites hash prefix — silently changes address identity); we fail-closed
    reject instead: safer for an indexer than synthesizing rewritten addresses; no
    fixture/vector/real-traffic carries anycast; anycast is near-dead in TON. Revisit
    trigger: a production anycast message that matters. (~15-line change + hash-rewrite
    helper if ever needed.)
13. Model routing (§3): T11.1 executes on opus against a lead-authored emitted-shape
    spec (golden "this ABI struct → exactly this C++ text" examples in the W11 wave
    plan); escalate executor to Fable-class ONLY on demonstrated struggle in first
    review round. No other task needs above opus — every other HIGH task copies a
    reference algorithm under vector gates.
14. Canonical value-JSON schema FROZEN AT W0 (vectors + AbiValue dump + generated
    to_abi_value all conform): bigints = decimal strings; addresses = raw "wc:hex" /
    "none" / null (maybe-absent); external address = {"extern": {"bits": N, "value":
    hex}} — bit LENGTH preserved (reference distinguishes same value at different
    widths, e.g. 1234@30 vs 1234@80 bits); cell = BOC base64 with PINNED serialization
    flags matching whatever @ton/core cell.toBoc() defaults the oracle emits (no index,
    crc32c, no cache bits — exact vm::std_boc_serialize mode determined + recorded in
    VALUE_SCHEMA.md at W0); slice-ish (bitsN/remaining) = {"bits": binary-str, "refs":
    [BOC-b64]}; map = array of [key, value] sorted by key; UNION follows the reference's
    hasValueField distinction (types-kernel.ts:225-276, dynamic-serialization.ts:391+):
    struct-labeled variants inline as {"$": "StructName", ...fields} with NO value
    wrapper; only primitive / duplicate-label variants get {"$": label, "value": ...};
    struct = object in field order; void = {"$": "void"}; enum = decimal string;
    bool = JSON bool; string = UTF-8 with @ton/core's lossy decode semantics
    (loadStringRefTail → buffer.toString); arrayOf/lispListOf/tensor/shapedTuple = JSON
    arrays; **cellOf = {"ref": inner}** (reference wraps — dynamic-serialization.ts:
    292-295; naive inner-only dump diverges on every cellOf vector); null = JSON null.
    The freeze BINDS TO the gen_vectors fixed serializer's output as validated by the
    T0.2 round-trip check; VALUE_SCHEMA.md documents it. Disputes resolved by the
    round-trip evidence, never ad-hoc.
15. Oracle reproducibility: tools/ has committed package.json + package-lock pinning
    @ton/tolk-abi-to-typescript, @ton/tolk-js, AND **@ton/core** exact versions
    (tolk-js absent from the reference repo's own lock; @ton/core is devDep-only there
    (package.json:46) so a consumer install may lack/drift it — and its serialization
    behavior IS part of the oracle); accepted abi_schema_version = exactly "1.0";
    vector file header records {pkg version, tolk-js version, @ton/core version,
    compiler version, schema version}; gen_vectors output byte-stable via a FIXED
    serializer — JSON object keys keep SCHEMA-DEFINED order (struct fields per §0.14);
    sorting applies only to map ENTRIES and metadata keys.
16. mch bridge integration shape (W12; REVISED review round 2): mch's MsgParserFn
    migrates raw-fn-pointer → std::function (mechanical mch-side change; call sites
    unchanged). Bridge owns loaded AbiModels (static, immortal, NO hot reload).
    **Registry identity = NAME, not opcode** (mch selects parsers by registry name from
    artifact parse stmts; it never dispatches by opcode — shared prefixes across
    standard ABIs are legitimate). Key = `<contract_name>::<decl name | mangled
    monomorph name>`; bodies WITHOUT a decl name (inline unions) REQUIRE an explicit
    manifest-supplied name. Duplicate registry keys = startup error. NO prefix-
    uniqueness enforcement (prefix check lives inside each parser only; revisit only if
    opcode-based dispatch is ever added). **T12.2 AMENDMENT (acked deviation): registry
    keys are BARE decl names, not contract::decl — the .mch grammar's parse-as reads a
    bare IDENT (`::` unlexable without grammar change); all pre-existing rows are bare;
    validate_registries() is the collision backstop. Revisit trigger = first real
    cross-ABI name clash.** **Deployment + init (required in W12 design
    section)**: ABI artifacts = committed dir passed via config/CLI arg (same pattern as
    the mch IR artifact; CMake install/packaging updated accordingly — current install
    ships exe only); explicit `abi_bridge_init(path) -> td::Status` called by BOTH the
    CLI and mch-classify embedding consumers BEFORE workers start; init failure aborts
    startup; registry access before init = hard error.
17. Emitter deliverables (beyond structs; required for A/B): generated PUBLIC HEADER
    (.h with struct decls + from_slice/store signatures) + per-struct
    `to_abi_value() const -> AbiValue` (§0.14; A/B gate + consumers). Custom-serializer
    mechanism (REVISED round 4 — TWO registries, mirroring the reference): the C++ value
    models genuinely differ per path (interpreter: Slice→AbiValue / AbiValue→Builder;
    generated: Slice→typed T / T→Builder), and no AbiValue→T conversion exists — so one
    shared store cannot serve both. (a) INTERPRETER registry:
    `register_abi_custom_dynamic(key, pack_fn, unpack_fn)` in AbiValue terms; (b)
    GENERATED registry: `register_abi_custom<T>(pack_fn, unpack_fn)` typed, type-erased
    behind the generated header's decl. BOTH: runtime lookup (missing = runtime
    "not registered" error, reference semantics — NOT template specializations / link
    errors), key = `<contract_name>::<decl name>` (§0.16 identity), pack and unpack
    INDEPENDENTLY optional (OnlyWithPack / OnlyWithUnpack cases). Covers custom
    STRUCTS, ALIASES, ENUMS. A/B custom cases: test setup registers on BOTH registries
    — following the reference's DYNAMIC-side registration list (NB upstream asymmetry:
    dynamicCtx deliberately does not register Color — LotsOfWrappers.spec.ts:376
    commented out).
18. Emitter STUB-vs-FAIL policy (final review F1 — without this W11 is unbuildable):
    per-direction CantGeneratePackUnpack-equivalent (non-serializable type in a
    from_slice/store body: bare int/slice/builder/callable/unknown) → emit a
    COMPILING STUB returning a runtime td::Status error (mirrors reference
    generate-ts-wrappers.ts:50-59,100-111 runtime-throw stubs). Decl-level
    NonStandardDictKey-equivalent → ton-abi-gen HARD FAILURE. Rationale: the main
    fixture deliberately contains non-serializable decls (NotPackable1-8,
    OnlyWithUnpack store) — hard-fail-on-any means generated/ can never be emitted;
    stub-everything means ErrorsAtCodegen can't pass. This split is the reference's.
19. Reference-emitter BUG — DO NOT PORT (final review F2): emitLoadOfStruct gates the
    custom-serializer path on pack_to_builder but invokes custom UNPACK
    (emit-pack-unpack.ts:11-12); dynamic path gates correctly on unpack_from_slice
    (dynamic-serialization.ts:344). DYNAMIC-path gates are authoritative for BOTH our
    consumers. Untested upstream (one-sided customs are aliases, not structs) so the
    A/B gate will NOT catch importing it — W11 adds one native unit: one-sided custom
    STRUCT, both directions.

## 1. Module layout

ABSOLUTE ROOT (pin — two ton-index-worker trees exist on this machine; the OTHER one,
S:\Dev\ton-index-worker, has NO ton-mch-engine and is the WRONG tree):
**S:\Dev\ton-indexer-clear\ton-index-worker**. All relative paths below anchor there.

```
ton-index-worker/ton-abi/
  CMakeLists.txt            — lib ton-abi + exe ton-abi-test + exe ton-abi-gen + ctest
  src/AbiModel.h            — ContractABI/Ty PODs (~30 kinds; mirrors abi.ts+abi-types.ts)
  src/AbiLoader.h/.cpp      — JSON → AbiModel; fail-closed (unknown kind/OOB index/missing
                              field/abi_schema_version pin = load error); bigint-string →
                              td::RefInt256
  src/AbiKernel.h/.cpp      — SymTable, aliasTargetOf, structFieldsOf (monomorph +
                              client_ty_idx cell-side + uLabelTyIdx), createLabelsForUnion,
                              load-time resolution pass (flatten interned indices → field
                              vectors + sorted prefix tables). NO calcWidthOnStack.
  src/AbiLeaves.h/.cpp      — L1 (split TUs as it grows): scalars, addresses ×4,
                              refs/nullable/string/remaining, arrayOf/lispList, dict,
                              prefix ops + union dispatch; every leaf = load+store pair
  src/AbiValue.h/.cpp       — interpreter value variant tree + canonical JSON dump
  src/AbiInterp.h/.cpp      — unpack+pack walkers over resolved model; custom-serializer
                              registry; field-path error chaining
  src/AbiEmit.h/.cpp+main   — ton-abi-gen: resolved model → C++ structs {static PREFIX,
                              typed fields, static td::Result<T> from_slice(vm::CellSlice&),
                              td::Status store(vm::CellBuilder&) const}; unions →
                              std::variant; nullable → td::optional; enum → enum class;
                              monomorphs → mangled concrete structs (Wrapper_int64);
                              --check repro mode
  generated/                — committed emitter output for testdata ABIs (A/B gate input)
  test/doctest.h            — vendored
  test/*.cpp                — TDD units per component + ported reference suites
  test/VectorRunner.cpp     — data-driven conformance: one doctest case per vector
  testdata/fixtures/*.tolk  — 26 fixtures copied from reference repo (MIT) + compiled ABI
                              JSONs (committed)
  testdata/abi_vectors.json — committed (golden fift-hex + expected-value JSON + error
                              cases incl. per-field truncation)
  tools/gen_vectors.mjs     — dev-only Node: imports @ton/tolk-abi-to-typescript +
                              tolk-js; compiles fixtures; dumps vectors; byte-stable
```

## 2. Test-port matrix (locked; from case-by-case catalog of all 14 reference specs, 231 runtime cases)

| Reference spec | cases | disposition |
|---|---|---|
| LotsOfWrappers | 87 | **PORT direct** — full serialization matrix; golden fift-hex (`x{..}`) asserts, double round-trip, full-consumption; static==dynamic equivalence becomes generated==interpreter; 30 exact-error asserts re-specified (mapping committed); 10 custom-serializer types via registry seam; `invalid-js-naming%` → C++ identifier-sanitization analog |
| ClientTypeAnno | 3/9 | PORT — cell-serialization + client_ty_idx ABI inspection; 6 getter/debug-print cases OUT |
| GenericUnionLabels | 2/10 | PORT — generic-union `$`-label serialization round-trip; getters OUT |
| LotsOfMessages | 1/2 | PORT — createCellOf hash equality (pure cells); TVM send case OUT |
| LotsOfStorage | 2 | PORT — default-value fill logic + ABI default_value ConstExpr trees |
| LotsOfThrows | 5 | PORT — isDefaultValueSupported unit, thrown_errors ABI parse, Errors-const analog |
| OnlyHeader + LotsOfAnnotations | 9 | PORT — ABI-parse asserts on committed JSONs (validates our loader) |
| ErrorsAtCodegen | 2 of 4 | PORT the 2 err-invalid-map-key cases (decl-level hard fail); the 2 err-cont-on-stack cases are GET-METHOD errors (getter domain, OUT) — message-parsing emitter stubs `callable` per §0.18 instead of rejecting |
| Cli | 1 | PORT analog — ton-abi-gen CLI writes file containing struct signature |
| HasNotInitializedStorage | 3 | OPTIONAL (deploy-domain address derivation) — decide at W10 |
| LotsOfGetters 73 · TolkCounter 3 · DebugPrintDemos 28 · getter halves | ~110 | **OUT** — getter scope; future path §5 (in-tree emulator + stack-snapshot vectors) |

In-scope total ≈ 118 cases = 100% of message-parsing behavior in the reference suite.
COUNT CAVEAT (round 3): case numbers tally runtime assertions/fixtures, not literal
it()-blocks (specs = 158 blocks); treat all counts as approximate — executors scope from
the actual spec files, not this matrix.

## 3. WAVE / TASK MAP

Difficulty→model: LOW→haiku-class, MED→sonnet-class, HIGH→opus-class (§0.13 for T11.1).
Scope: S <150 LOC, M 150-500, L >500 incl. tests. Lead verifies fresh + commits per wave;
teammates never commit. Wave-boundary ops (proven): executor rotation on long context
(handoff doc, TaskStop before respawn), STOP-and-report over tweak-toward-green.

| Wave | Task | Content | Diff | Scope | Model | Status |
|---|---|---|---|---|---|---|
| **W0 scaffold** | T0.1 | CMake lib+test exe+ctest, vendor doctest, smoke green | LOW | S | haiku/sonnet | **DONE** |
| | T0.2 | gen_vectors.mjs + committed tools/package{,-lock}.json (§0.15; if the pkg is unpublished use file:/git dep, version = git hash); compile 3 fixtures, emit ABIs + first golden-hex vectors; SELF-VALIDATION REQUIRED (final review F4): (a) every emitted value-JSON re-parsed → dynamicPack → hex must equal the golden hex (lossless-serializer proof); (b) cases transcribed from reference specs assert the spec's LITERAL golden hex at generation time; (c) port the specs' custom-serializer TS impls (LotsOfWrappers.spec.ts:304-380) into gen_vectors. Write testdata/VALUE_SCHEMA.md — documents the serializer's validated output (§0.14), does NOT independently legislate | MED | M | sonnet | **DONE** |
| | T0.3 | VectorRunner skeleton: one doctest case per vector, vectors carry `enabled_from: <wave>` gating — runner GREEN on empty enabled set (green-at-boundary holds); waves enable their slices | LOW | S | haiku/sonnet | **DONE** (runner later RETIRED at W11.5 with the interpreter — A/B gate took over) |
| **W1 model+loader** | T1.1 | AbiModel PODs, all ~30 Ty kinds + ContractABI arrays (mechanical mirror) | LOW | M | sonnet | **DONE** |
| | T1.2 | AbiLoader TDD: fail-closed walk; reject units — SEMANTIC invariants too: unknown kind, any nested index OOB, missing field, version ≠ "1.0", malformed bigint, duplicate decl names, instantiation arity mismatch, intN/uintN/bitsN width limits, alias cycles, union PREFIX-FREEDOM (no variant prefix strict-prefix of another — makes W2's sorted dispatch provably equal to reference declaration-order first-match), prefix bounds CORRECTED at W1: UNION-VARIANT prefix_len ≤ 32 (float64 rationale holds there), but STRUCT-level prefixes go WIDER (48-bit exists in lots-of-wrappers MsgSinglePrefix48) → struct bound = ≤1023 + uint64 prefix_num | MED→HIGH | M | sonnet (opus if invariant set balloons) | **DONE** |
| | T1.3 | Loads-all-fixture-ABIs test (full set) | LOW | S | haiku | **DONE** (18/19 compile; small.tolk = upstream compiler error; err-* compile to VALID ABIs → committed as W11 reject-testdata) |
| **W2 kernel** | T2.1 | SymTable + aliasTargetOf + resolution pass (flatten fields, sort prefix tables) TDD. THREE NAMED INVARIANTS (final review F3): (i) union label/prefix tables keyed per (union ty_idx, uLabelTyIdx) — labels are a USE-SITE property, same interned union can arrive with different uLabel from different instantiations; (ii) custom-hooked alias/enum nodes SURVIVE resolution as nodes (reference checks custom_pack_unpack BEFORE aliasTargetOf — dynamic-serialization.ts:335-374); (iii) prefix-freedom validated BEFORE sorting (makes sort provably behavior-preserving vs reference's declaration-order first-match) | MED→HIGH | M | **opus** (was sonnet — invariants subtle) | **DONE** |
| | T2.2 | structFieldsOf monomorph + client_ty_idx (cell side) + uLabelTyIdx + createLabelsForUnion; units ported from types-kernel behavior + generic fixtures; NB nullable DROPS uLabelTyIdx per reference (dynamic-serialization.ts:291, emit side agrees) — do not "fix" | **HIGH** | M | **opus** | **DONE** |
| **W3 scalars** | T3.1 | intN/uintN/bool/coins/varintN/varuintN/bitsN load+store TDD, boundary units (min/max/truncation) | MED | M | sonnet | **DONE** |
| **W4 addresses** | T4.1 | address/maybe_address/external_address/address_any load+store — @ton/core accept/reject parity + source-derived reject-space units | **HIGH** | M | **opus** | **DONE** (anycast = known-divergence, see log + §0.12b) |
| **W5 refs+strings** | T5.1 | cell/cellOf/nullable(1-bit)/maybe_ref; snake string ref-tail (normative-excerpt rule applied); remaining snapshot; BOC-flags pin (W0-deferred §0.14 item) | MED | M | sonnet | **DONE** |
| **W6 containers** | T6.1 | arrayOf (TS-parity store / dual-format load), lispListOf, dict adapter | **HIGH** | M | **opus** | **DONE** (dict labels PROVEN identical vm↔core — no custom writer needed) |
| **W7 prefixes** | T7.1 | load_and_check_prefix (any len) + lookup peek + union prefix-tree dispatch + store side | **HIGH** | M | **opus** | **DONE** (leaf layer complete) |
| **W8 interpreter** | T8.1 | AbiValue tree + canonical JSON dump | LOW | M | sonnet | **DONE** |
| | T8.2 | Unpack+pack walkers + dynamic custom registry + exotic reject + field-path errors | MED | M | sonnet | **DONE** |
| | T8.3 | Vectors LIVE: 12→30 both directions + lossy UTF-8 + error vectors | MED | M | sonnet | **DONE** (gaps flagged: shapedTuple walker-level only; addressExt nibble-aligned; custom vectors = W9) |
| **W9 suite port (INTERPRETER-ONLY)** | T9.1 | Deduplicated LotsOfWrappers port: error mapping + 9 customs + representative units + 3 custom vectors | MED | M | sonnet | **DONE** |
| | T9.2 | Carve-outs: ClientTypeAnno client_ty_idx cell path, GenericUnionLabels literals, createCellOf hash-eq (default-fill case → W11 as planned) | MED | M | sonnet | **DONE** |
| **W10 ABI-inspection** | T10.1 | OnlyHeader/LotsOfAnnotations/LotsOfThrows/LotsOfStorage-c2 asserts + enum-description loader patch | LOW | M | haiku/sonnet | **DONE** (HasNotInit derivation → deferred register) |
| **W11 emitter** | T11.1 | ton-abi-gen core per lead shape spec | **HIGH** | **L** | **opus** | **DONE** |
| | T11.2 | 17 generated pairs (18.3k LOC, all compiled) + --check ctest + FULL A/B (33 vectors, static==dynamic) + default-fill + MyBorderedInt typed | MED | M | opus (continued) | **DONE** |
| | T11.3 | ErrorsAtCodegen pins (2 hard-fail + 2 emit-cleanly) + Cli-analog | LOW→MED | M | opus (continued) | **DONE** |
| **W12 mch bridge** (lives in ton-mch-engine; full design section in wave plan per §0.16 BEFORE code) | T12.1 | MsgParserFn → std::function migration (TWO tables: MsgParse.cpp map + MsgParsersGenerated registry vector; NB message_parsers() is a Meyer's-static built on first call — restructure init or add a second registry, AND enforce §0.16 access-before-init error AT THE ACCESSOR: pre-init callers exist — BuildDriver artifact validation, --surface, ParseDump) + AbiValue→mch::Value adapter + registry rows per §0.16; all mch gates untouched | MED→HIGH | M | **opus** | **DONE** — BUT NOT AS THIS ROW PRESCRIBES: W11.5 obsoleted the std::function/init machinery; as-built = W12-BRIDGE-DESIGN.md (raw-fn-ptr template trampoline, static rows, validate_registries) |
| | T12.2 | First new protocol end-to-end: .tolk decl → ABI → classified action. NB --surface/cpp_surface.json UPDATES here (gates-untouched applies to T12.1 only; normal protocol-addition procedure) | MED | M | sonnet | **DONE** (DeDust V2 claim_reward; as-built = T122-CLAIMREWARD-DESIGN.md §10; ran opus) |

## 4. TDD protocol (every code task)

1. Failing doctest unit (hand-built cells via vm::CellBuilder) → RED. 2. Minimal impl →
GREEN. 3. Store-side mirror, same cycle. 4. Vector slice ONLY where a driver exists:
ABI-parse vectors enable at W1; value/hex vectors enable at W8 (the interpreter walker is
their driver — W3-W7 leaf waves are doctest-unit-ONLY, no vector step). 5. Lead re-runs
full ctest fresh + commits. Conformance layer independent of units; VectorRunner failure
output names fixture + field path.

## 5. Deferred register (additive later; nothing in core needs rework to add)

- Getter/stack path: calcWidthOnStack, StackReader, stack type-id unions, wide-nullable
  stack, getter wrappers in emitter, debug-print (28 cases via committed TVM-stack
  vectors), LotsOfGetters 73 + TolkCounter 3 via in-tree external/ton emulator harness
  (~300-500 LOC). Trigger: first consumer needing getters.
- HasNotInitializedStorage address/shard derivation (SKIPPED at W10 per delegated
  criterion: toShard fixedPrefixLength+closeTo = real shard-prefix-rewrite math, not
  trivial with existing machinery). Also from W8: shapedTuple dedicated vector
  (walker-level covered; Recurrent fixture self-referential), addressExt augmented-hex
  (non-nibble-aligned) width vectors.
- tondb-scanner tokens::gen migration; official third-party ABI consumption.
- Upstream gaps inherited: generic-union reshape, non-standard map keys, custom
  serializers on generics.
- Compiler-executed arrayOf vectors (store-on-TVM via sandbox → committed chunked cells)
  — trigger: arrays appear in a real protocol message we must unpack (§0.12).

## R6. FINAL review round 5 — Fable (2026-07-16) — RESOLVED; VERDICT: EXECUTE

1 P1 + 4 P2 + 5 P3, all doc-level, all folded: **F1** stub-vs-fail policy (§0.18 — the
W11 unbuildability catch: main fixture deliberately contains non-serializable decls);
**F2** reference-emitter wrong-flag BUG pinned do-not-port (§0.19 — A/B gate cannot
catch importing it); **F3** three W2 resolution invariants named (per-use-site labels,
custom-hooks-survive-resolution, prefix-freedom-before-sort; nullable DROPS uLabel —
don't fix) — T2.1 upgraded to opus; **F4** gen_vectors self-validation (round-trip
lossless proof, literal-gold asserts, custom impls ported, dynamic-side registration
list); **F5** §0.14 completed (cellOf {"ref":...}, string lossy-UTF8, bool, arrays;
freeze binds to validated serializer output); **F6** ErrorsAtCodegen 2/4 (cont-on-stack
= getter domain, stubs per §0.18); **F7** prefix-freedom + len≤32 loader checks; **F8**
dict-pack label-parity policy in W6; **F9** W12 Meyer's-static + pre-init callers +
T12.2 surface-gate note; **F10** file:/git oracle fallback. §R.1 answered (normative
@ton/core excerpts + reject vectors), §R.3 (flatten OK under F3 invariants), §R.4
(split right, round-trip makes sides mutually checking), §R.6 (exotic-cell reject at
top-level open added T8.2; nothing else missing). Hard-case walk (monomorph → nullable
→ generic union → custom alias variant) closes with F2/F3/F5 landed.

## R5. Plan-review round 4 (2026-07-15) — RESOLVED

2 P1 (both confirmed valid on check, not false positives): (1) single custom registry
cannot serve both value models (no AbiValue→T conversion exists; round-3 "one store"
was a hole, not an improvement) → §0.17 now mirrors the reference: TWO registries
(dynamic/AbiValue + generated/typed-T), shared key rule, independently-optional
pack/unpack; (2) @ton/core pinned explicitly (devDep-only upstream; its behavior IS the
oracle) + version in vector metadata (§0.15). P2: struct-field-order vs sorted-keys
conflict resolved (schema order for objects; sorting = map entries/metadata only).
P3 ×2: decision 9 ".h+.cpp"; stale §R question marked resolved.

## R4. Plan-review round 3 (2026-07-15) — RESOLVED; VERDICT: EXECUTE

No P1. 4 P2 (all doc-text): stale trait wording in T11.1/T11.2 → runtime registry;
§0.14 union rule corrected to the reference's hasValueField distinction (struct variants
inline, no value wrapper); §4/T0.3 vector-enabling clarified (ABI-parse @W1, value/hex
@W8, leaf waves unit-only); absolute root pinned in §1 (two ton-index-worker trees
exist). 4 P3 resolved: tensor/shapedTuple assigned to T8.2 walker; custom store
contract-scoped (§0.17); counts marked approximate (§2); T9.1 DEDUPLICATED (golden-hex
bulk stays VectorRunner-only; native units = error asserts + custom + ~10
representative). Reviewer verified sound: §0.12 array policy vs reference load loop,
§0.16 fn-pointer/name-keyed premises (2 tables noted in T12.1), §0.17 runtime-error
semantics, version pin, all §B/§C citations.

## R3. Plan-review round 2 (2026-07-15) — RESOLVED

3 P1 + 2 P2, all accepted: (1) bridge collision identity corrected opcode→NAME (§0.16:
key = contract::decl/mangled name, manifest name for inline unions, duplicate keys =
startup error, prefix-fatal dropped — mch never opcode-dispatches); (2) abi_custom
specialization model replaced with runtime typed registry shared with interpreter
(§0.17 — test-TU specializations aren't visible to generated TU; runtime not-registered
error preserves reference semantics; covers structs/aliases/enums); (3) ABI artifact
deployment + init API specified (§0.16: config/CLI path like the IR artifact, CMake
install update, abi_bridge_init before workers, guarded registry); (4) stale T6.1 array
text aligned to §0.12; (5) extern-address JSON made width-preserving + BOC flags pinned
(§0.14).

## R2. Plan-review round 1 (2026-07-15) — RESOLVED

External review found 4 P1 + 3 P2; all accepted and folded in: (1) arrayOf pack flipped
to TS parity so the oracle stays authoritative (§0.12); (2) green-at-boundary restored —
VectorRunner GREEN-empty with per-wave `enabled_from` gating, W9 interpreter-only,
generated-equivalence + default-fill moved to W11; (3) W12 bridge design pinned (§0.16:
std::function migration, static model ownership, naming, prefix-collision FATAL, no
reload); (4) emitter deliverables expanded (§0.17: public header, to_abi_value(),
abi_custom<T> trait); (5) value-JSON schema frozen at W0 (§0.14 + VALUE_SCHEMA.md);
(6) oracle pinning (§0.15); (7) loader semantic-invariant list (T1.2).

## R. Reviewer guidance — what to challenge

1. (RESOLVED final review) Vectors alone insufficient for the accept/REJECT boundary —
   W4/W5 briefs carry @ton/core implementation excerpts as normative + reject-space
   error vectors. Answered in T4.1/W5 rows.
2. (RESOLVED round 3 — T9.1 deduplicated; kept for history) W9 duplication question.
3. **Resolution-pass design** (W2): flattening at load vs resolving lazily — hidden
   coupling risk with emitter (W11 consumes the same resolved model)?
4. **AbiValue JSON dump as conformance medium**: canonicalization pitfalls (bigint
   rendering, address formatting, map ordering) — is fift-hex comparison on PACK side +
   value-JSON on UNPACK side the right split?
5. **Wave granularity**: W3-W7 = 5 leaf waves; too fine (overhead) or right (each HIGH
   leaf isolated)?
6. **Missing**: anything the message-parsing scope needs that neither the matrix nor the
   deferred register names? (e.g. BOC parsing entry — bodies arrive as Ref<Cell> from
   existing pipeline, so no BOC decode in ton-abi — confirm.)
7. Estimates sanity (§D effort basis; L1 600-800, loader 500-600, kernel 300-400,
   walkers ~500, emitter 800-1,200).

## 6. Wave log (append per wave; status column above updates same commit)

- **R1a (refactor wave, ton-abi module) DONE 2026-07-25** (from 3-reviewer quality pass;
  lead-verified fresh: ton-abi ctest 2/2, ctest mch 8/8, classify twin diff-0 at 199).
  Landed: A/B gate HARDENED (vector count==33 + dispatch-key-consumption asserts —
  previously an empty vector file passed; proven to bite on truncated copy); dead
  interpreter union-dispatch deleted (dispatch_union/store_union_prefix + UnionDispatch)
  which was the SOLE edge pulling AbiKernel into generated TUs — include audit via
  /showIncludes proves generated code now reaches ONLY AbiGenSupport+AbiValue+leaf
  headers (invariant restored); union-form coverage ported to generated path BEFORE
  deletion (7 subcases; known gap flagged: emitter's T|void implicit-EAT branch has no
  fixture — uncovered before too); 6 hand-copied oracle goldens deleted from
  AbiEmitTest; enabled_from purged from all 33 vectors (dead since VectorRunner
  retirement); PROD ABI RELOCATED testdata/fixtures/ → ton-abi/abi/ with standalone
  tools/gen_prod_abi.mjs (tolk-js only — prod ABI regen no longer needs the reference
  repo checkout; --check covers abi/ + CONFIGURE_DEPENDS); custom-registry emission
  deduped 12→1 helper with cpp_str_lit escaping (9 unescaped sites fixed; --check
  byte-identical proof); shared test/AbiTestSupport.h (13 TUs, CHECK-macro dance
  centralized); doc batch: W11-SHAPE-SPEC §5 enum paragraph rewritten (was mandating
  member-set validation the REFERENCE DOESN'T DO — oracle-parity trap), 6 spec-vs-code
  fixes, custom doc-comment requirement dropped, VALUE_SCHEMA marked historical →
  AbiValue.h table, W8-HANDOFF salvaged into MODULE-NOTES.md + deleted, interpreter
  framing purged from live comments/test names. Doctest 162→153 cases (net −9, per-case
  disposition in wave report; every deletion mapped to retained coverage). NB build:
  never build `all` target (pre-existing protobuf×MSVC C2139); named targets only.

- **W12/T12.2 DONE 2026-07-24** (FIRST REAL PROTOCOL through ABI path: DeDust CPMM V2
  claim_reward; design doc `ton-mch-engine/T122-CLAIMREWARD-DESIGN.md` + §10 as-built;
  lead-verified fresh ctest mch 8/8 + ton-abi 2/2; executor full gate run: classify/
  actions/match/parse-dump/schema/prod ALL diff-0 at **196→197** actions (+1 = resident
  jetton fixture reclassified unknown→dedust_v2_claim_reward), pytest 1988/2skip).
  Proven end-to-end: minimal decl `dedust_v2.tolk` (3 structs, plan §0.11 flow — NOT
  the full 943-line .talk) → committed ABI JSON → generated pair (--check covered) →
  3 PROD rows in abi_message_parsers() (first non-empty; validate_registries dup-check
  now live) → spec `dedust_v2_claim_reward.mch` (tolk-universe camelCase captures) →
  action `dedust_v2_claim_reward` on dex_withdraw_liquidity_data (+reward_index extra).
  Python twin = hand gate-only parsers in builders/dedust_v2.py mirroring the adapter
  shape; ADDRESS PARITY PROVEN (AccountId.as_str().upper() == make_account_raw render,
  unit + pipeline). Composite ported both sides (C++ Action field + render columns).
  BARE registry names per §0.16 amendment. TON-payout leg unit-gated only (fixture
  fetch impossible from this tree — no kvrocks-capable save_trace/VPN; YAML case
  follows when fixture lands). Fixture-swap RESOLVED post-wave (user chose keep-both):
  02kEPzkQ restored from git + i-gQwY3n COMMITTED → corpus 148, baseline 199 (197 +2
  from restored fixture), classify/actions/match re-verified diff-0 at 148.

- **W12/T12.1 DONE 2026-07-24** (mch bridge infra; lead-verified fresh: ctest mch 8/8
  incl. new mch_abi_bridge 37 checks, ton-abi 2/2; executor-run full §1 gate set all
  diff-0 + Python 1988 pass). Design-first per §0.16 with acked reconciliation doc
  `ton-mch-engine/W12-BRIDGE-DESIGN.md`: §0.16's interpreter-era machinery DROPPED
  (std::function migration, abi_bridge_init, ABI-artifact dir, install change — pure
  codegen made registration compile-time static; `&abi_parse_body<T>` template
  instantiation IS a raw MsgParserFn, both existing tables untouched); KEPT name
  identity `<contract>::<Decl>`, dup-key-is-error (reshaped: validate_registries()
  advisory at CLI startup + fail-closed ambiguous-name error in parse_message_body),
  exotic-cell reject re-homed to trampoline open. Adapter = total AbiValue→mch::Value,
  tolk-faithful NEW value universe (Extern→Obj{bits,value}, Bits→Cell, Map→list-of-
  pairs, String→raw bytes, AddrNone→account_none, struct-labeled unions inline);
  no new VType. Fixture pairs (tolk_counter, jetton_wallet_contract, lots_of_wrappers)
  compiled into HARNESS EXE only — prod abi rows EMPTY. mch-classify now PUBLIC-links
  ton-abi. Bridge ~620 LOC. REMAINING in W12: T12.2 first real protocol row
  (+ --surface update + embedding-actor validate seam noted).

- **W11.5 DONE 2026-07-16** (refinement wave; lead-verified fresh: ctest 2/2 [doctest
  162 cases / 1579 asserts + gen_check --check], 33 vectors both directions via
  GENERATED path [368 asserts incl. expect_error], grep AbiInterp clean).
  **DIRECTION AMENDMENT (user): the interpreter was DEV SCAFFOLDING, never a product
  feature — end state = PURE CODEGEN.** Supersedes the design doc's "interpreter stays
  forever as A/B twin" framing (lead's misread of intent; truth was always the TS
  oracle vectors, and the A/B gate runs generated-vs-vectors directly). Landed:
  (1) TYPED INT WIDTHS — intN/uintN n≤64 → native td::int64/td::uint64 members
  (uniformly: fields, container elems, map keys/values); new leaves
  load/store_{int64,uint64} via native fetch, bit-identical to RefInt256 path (hash-eq
  units at n=1/32/63/64); coins/varintN/n>64 stay RefInt256 (range-honest); ENUMS stay
  RefInt256 (bigint-valued free helpers per W11 correction; member shape unaffected);
  ripple: Custom8 (alias uint8, custom) registry type → td::uint64.
  (2) INTERPRETER RETIRED — AbiInterp.{h,cpp} + dynamic custom registry + VectorRunner
  + AbiInterpTest + AbiInterpW9Test DELETED (−3,098 LOC hand-written, net −2,855);
  single TYPED registry remains; AbiValue RE-SCOPED to test-gate dump + future
  mch-bridge interchange (not in parse hot path); 2 unique W9 behaviors PORTED to
  generated path (client_ty_idx cell-side swap incl. static_assert member type;
  Color declared-not-registered runtime error); all other deletions = dup-of-A/B or
  interpreter-only concepts (pack-from-AbiValue errors, top-level exotic-reject →
  bridge concern, noted for W12); ERROR_MAPPING.md re-homed accordingly.
  (3) to_abi_value slimmed (infallible leaves emit direct add_field, no TRY ceremony).
  Case count 179→162 with full per-case disposition table in the wave report; usage
  wart killed: `msg.queryId` is now plain int64 for ≤64-bit fields (was RefInt256
  ceremony). W11-SHAPE-SPEC §3 amended. STILL PAUSED before W12 per user order.

- **W11 DONE 2026-07-16** (3 sub-stages; final lead-verified: ctest 2/2 [doctest 179
  cases / 2576 asserts + ton_abi_gen_check --check byte-repro of all 17 pairs], all 17
  generated pairs COMPILED into the test exe, 18,308 generated LOC). ton-abi-gen
  emitter per lead shape spec: typed structs, BAKED dispatch (generated code never
  links kernel/loader), per-direction custom gates (§0.19 reference bug not ported),
  stub-vs-fail (§0.18 proven: err-map-key hard-fails, err-cont-on-stack emits stubs
  cleanly), create()+defaults (exactness incl. ±2^100 bigints), to_abi_value.
  **FULL A/B GATE GREEN: all 33 vectors through generated structs == expected ==
  interpreter (static==dynamic equivalence), + repack hash round-trips, + expect_error
  cases; 3 custom vectors via TYPED registry.** SPEC CORRECTIONS (T11.1, lead
  accepted — spec was wrong, not executor): enums = RefInt256 + free helpers (int256
  encoded_as exists; matches reference bigint repr); NO enum member-set validation
  (reference does none; adding would break A/B — proven with out-of-range vector);
  void variant = monostate. EMITTER RULES ADDED UNDER FIRE (T11.2, all gate-covered):
  fully-custom decl → type-erased `using N = AbiValue` + registry<AbiValue> (typed
  form impossible: TelegramString target=slice, CustomPoint empty struct); CellOf<T>
  holds shared_ptr (recursive types); MSVC can't parse nested td TRY_* at depth-3 →
  raw try-binds with unique temps; C++-keyword field names escaped (field named
  `void` exists); quote-bearing field names via cpp_str_lit; fixture-stem filenames
  (contract_name collides: both err fixtures = "Err"). Executor abi-w11 retired.
  **PROGRAM PAUSED here by user order — W12 (mch bridge) NOT dispatched; awaits
  explicit go.**

- **W10 DONE 2026-07-16** (lead-verified: 165 cases / 2099 asserts / ctest 1/1). All
  ABI-inspection specs ported onto committed fixtures (compiler_version PINNED "1.4.1",
  full ConstExpr tree shapes incl. big-bigint + nested shapedTuple); HasNotInit trimmed
  to the ABI-parse fact (derivation → deferred register); isDefaultValueSupported
  noted for W11 (codegen concern). LOADER GAP found + PATCHED same wave: ABIEnumMember
  description was silently dropped (model field + parse_enum read + real-value assert
  added). Executor delivered 5 emitter-facing observations → W11 shape spec inputs:
  (1) client_ty_idx swap direction reads backwards — kernel swaps TO client type on
  CELL side; (2) monomorphs resolve by ty_idx never name; C++ ident needs mangling
  (ResetTo<int64> → ResetTo_int64); (3) interpreter's string-label union pack matching
  is NOT transferable — generated code dispatches via std::variant visitor; (4)
  two-registry split confirmed necessary in practice; (5) non-idempotent custom
  serializers exist (MyBorderedInt) — generated-code doc-comment honesty.

- **W9 DONE 2026-07-16** (combined T9.1+T9.2, lead-verified: 160 cases / 1841 asserts /
  ctest 1/1 / 33 vectors / regen stable). testdata/ERROR_MAPPING.md committed: 20/27
  spec error asserts ported as CATEGORY checks; 6 N/A (JS duck-typing artifacts
  unrepresentable in a C++ tagged variant); 1 structurally impossible (Union kind can't
  lack inner). All 9 custom serializers ported + Color asymmetry preserved (unit proves
  declared-but-unregistered → error). 3 custom VECTORS added (closes W8 flag);
  MyBorderedInt deliberately vector-less — its encoding is NON-IDEMPOTENT (pack(20)→
  unpack→10, pack(10)→different tag: 10 not a fixed point) which by design breaks the
  harness round-trip self-check; native 3-range unit instead. **KNOWN-DIVERGENCE #3
  (ERROR_MAPPING.md)**: reference allows PACKING raw builder/slice via
  storeBuilder/storeSlice passthrough; we reject both directions symmetrically (plan
  decision 1 non-serializable bucket); no fixture/vector needs it; revisit trigger
  named. client_ty_idx CELL-SIDE swap confirmed live end-to-end (ClientPayload.note
  'remaining'→'string' serializes as snake ref-tail — matches reference spec assert).
  createCellOf ResetTo case needed the monomorph instantiation ty_idx directly
  (generic decl has no serializable ty of its own — expected per §B).

- **W8 DONE 2026-07-16** (3 sub-stages, lead-verified each; final: 152 cases / 1497
  asserts / ctest 1/1 / 30 vectors green BOTH directions / regen byte-stable).
  T8.1 AbiValue 13-kind tree + canonical dump (design call accepted: Union kind only
  for has_value_field variants — struct-labeled variants ARE their inner value,
  dump-identical). T8.2 AbiInterp walkers (full TyKind switch; AbiInterpCtx dynamic
  custom registry gated BEFORE resolution at GENERIC decl only — upstream limitation
  inherited by design; union pack matches Null-shortcut/Void/label+struct-name;
  exotic-cell top-level reject; real-fixture round-trips byte-exact).
  T8.3 vectors 12→30 (address/nullable/cellOf/array/lisp/tensor/mapKV incl.
  address-keyed/enum/generic-union labels T1-T2 proven/monomorphs + 3 error vectors:
  truncation, prefix mismatch, union no-match via hand-authored
  w8-hand-union.abi.json — REAL tolk unions are structurally exhaustive, dispatch gaps
  need hand fixtures). LOSSY UTF-8 implemented dump-side, verified byte-for-byte vs
  live Node on every edge class (truncations per position, lone continuation, overlong,
  surrogates, out-of-range; the reprocess-vs-consume subtlety documented); adversarial
  malformed-string vector = unpack_only (lossy ≠ invertible). GOTCHAS for posterity:
  vm::CellBuilder::finalize() is CONSUMING (2nd call → empty cell — Built{} helper
  pattern); T0.2(a) round-trip self-validation caught 2 oracle-side generator bugs
  (mutable-loop-bound on remainingBits; union re-wrap before packToBuilderDynamic) —
  the round-4 F4 requirement earned its keep. FLAGGED GAPS: shapedTuple vector-level
  (walker-tested only — Recurrent fixture self-referential), addressExt augmented-hex
  widths, custom-serializer vectors → W9.

- **W7 DONE 2026-07-16** (lead-verified: 114 cases / 691 asserts / ctest 1/1). LEAF
  LAYER COMPLETE (W2-W7). AbiLeavesPrefix.{h,cpp}: load_and_check_prefix/store_prefix
  (uint64 num, len≤1023, >64 high-bits-zero), lookup_prefix peek, dispatch_union,
  store_union_prefix. REFERENCE SEMANTICS PINNED (the verify-and-cite asks): (1) VOID
  ORDERING IS FORM-DEPENDENT — exact T|void: empty-check FIRST then variant[0]
  UNCONDITIONALLY (discriminator = emptiness, no prefix probe, ds.ts:382-384); general
  form: void LAST after all prefixes fail AND slice empty, else hard error (:408-410).
  (2) Either bool fast path = PURE optimization of the static-codegen path; dynamic
  path has NO special case → general loop only implemented. (3) Load: implicit EATEN
  by dispatch, explicit PEEKED-left-in-place (variant's own loader re-reads) — proven
  by cs.size()-unchanged test. (4) Store: union writes prefix ONLY when implicit;
  explicit written by the variant's own store. **W8-HANDOFF.md committed** (inherited
  API surface, W6 callback contract, 12 non-obvious gotchas incl. vm-hex-UPPERCASE-vs-
  JSON-lowercase) — DELETED in R1a once its wave-scoped brief had aged out; the durable
  parts live on in `ton-index-worker/ton-abi/MODULE-NOTES.md`. Executor abi-w2 retired
  after 6 waves (W2-W7), rotation at the clean leaf/walker boundary.

- **W6 DONE 2026-07-16** (lead-verified: 103 cases / 620 asserts / ctest 1/1).
  AbiLeavesContainer.{h,cpp} (arrayOf/lispListOf) + AbiLeavesDict.{h,cpp}. Structural-
  walker API: element (un)pack CALLBACKS (inner recursion = W8's; dict yields key+value
  slices). **DICT-PACK PARITY PROVEN — F8 resolved, NO custom writer needed**:
  vm::Dictionary+append_dict_to_bool labels == @ton/core serializeDict, proven by root
  CELL-HASH equality ({1,5,3} map + empty; hash not BOC bytes — BOC framing order may
  differ, cell tree = label identity is what golden fift-hex sees). **DICT ORDER
  PINNED**: both sides iterate HashmapE raw-key-bit ascending — neither re-sorts;
  signed intN keys: negatives sort LAST both sides (two's-complement); if signed-
  numeric JSON ordering ever wanted → W8 dump-side decision, wire unaffected. arrayOf
  load = reads-next-ref-FIRST-then-drains-bits shape (accepts both formats, count
  must equal len); lisp cons chain delivers reversed for natural append; inherited
  upstream limitation: lispList element types with own refs don't round-trip (tail =
  ref[0]). Pack byte-identity proven vs Node for arr/lisp incl. empties. W8 note:
  callback signatures are the walker contract (flag if different shape wanted).

- **W5 DONE 2026-07-16** (lead-verified: 89 cases / 567 asserts / ctest 1/1).
  AbiLeavesRef.{h,cpp}: cell/ref_slice(cellOf)/maybe_ref/maybe_prefix(nullable bit)/
  string snake/remaining. **BOC FLAGS RESOLVED (W0-deferred §0.14 item):**
  @ton/core toBoc() defaults {idx:false, crc32:true} ≡ vm::std_boc_serialize(root, 2)
  (Mode::WithCRC32C) — BYTE-IDENTITY PROVEN vs Node on hand-built + 3 snake cells;
  recorded in VALUE_SCHEMA.md; W8 cell dump = b64(std_boc_serialize(root,2)), oracle-
  agreeing by construction. Snake string semantics pinned from strings.js: byte-ALIGNED
  hard reject (no padding tolerance — loadPaddedBits is a different BOC-internal
  method), ≤1 continuation ref, 127-byte/cell writer chunking. Leaves belt-and-
  suspenders reject exotic cells via load_cell_slice_special (top-level reject still
  W8's). remaining = snapshot-and-drain (matches core clone-then-consume).

- **W4 DONE 2026-07-16** (lead-verified: 74 cases / 495 asserts / ctest 1/1).
  AbiLeavesAddress.{h,cpp}: AbiAddress{None|Std|Extern; wc int8-checked; hash Bits256;
  ext_value = bit-slice up to 511 bits (WIDER than RefInt256 — hence slice not int)} +
  4 load/store pairs; @ton/core tag map (0 none/1 extern/2 std/3 var) cited in header;
  19 units incl. one per source-derived reject condition (anycast, addr_var ×2 paths,
  truncations, wrong-tags, store rejects). DISCREPANCY SURFACED + DECIDED: plan brief
  "anycast REJECT" vs @ton/core actual behavior (processes + rewrites hash prefix) —
  executor followed plan, reported honestly; lead kept REJECT as known-divergence #2
  (§0.12b: identity-rewriting is worse than rejection for an indexer; revisit trigger
  named). Round-trips verified byte-identical via Cell::get_hash.

- **W3 DONE 2026-07-16** (lead-verified: 55 cases / 387 asserts / ctest 1/1).
  AbiLeavesScalar.{h,cpp}: int/uint/bool/coins/varint/varuint/bits load+store over
  vm::CellSlice/CellBuilder, @ton/core-normative (BitReader/BitBuilder.js read + cited).
  PARITY NUANCES pinned by tests: (1) varint size-byte count = @ton/core-exact
  ceil((mag_bits+1)/8) — DIVERGES from two's-complement bit_size at exact powers of two
  (-128 → 2 bytes); pack-parity-critical at W8. (2) var prefix width = log2(n) verified
  exact (fail-closed non-power-of-2; real ABIs emit 16/32). (3) bitsN value =
  Ref<CellSlice> bit-string (slice-ish §0.14 kind), not RefInt256; {bits,refs} dump =
  W8's job. Leaves take Tolk width n, compute prefix internally.

- **W2 DONE 2026-07-16** (lead-verified: 38 cases / 285 assertions / ctest 1/1,
  green-empty runner intact). AbiKernel.{h,cpp} landed: SymTable (per-kind, verbatim
  names — see correction below), aliasTargetOf, struct_fields_of (cell side),
  create_labels_for_union + resolve_union{variants decl-order, dispatch_order sorted,
  has_void}. INVARIANTS realized: (i) eager caches ONLY for use-site-independent data
  (fields/targets/prefix-dispatch per ty_idx); labels computed per (union ty_idx,
  uLabel) at CALL time, never cached under ty_idx alone — proven by test (same interned
  union: int32/int64 vs T1/T2); (ii) kernel never reads custom_pack_unpack — W8 walker
  MUST gate custom BEFORE alias/struct resolution (documented in header); (iii)
  dispatch sort's behavior-preservation dependence on the loader prefix-freedom check
  documented at build_union_dispatch. **CORRECTION to T2.1 row**: <...>-stripping does
  NOT belong in the kernel — StructRef/AliasRef names are ALWAYS plain (all fixtures);
  mangled names live only in struct_instantiations[].struct_name which the kernel
  matches by ty_idx; stripping stays in the loader's arity validation (W1). §R.3
  definitively resolved: eager flatten limited to use-site-independent data + per-call
  labels = no walker duplication, thread-safe post-create. ResolvedField keeps pointer
  into ContractABI (abi must outlive kernel — matches §0.16 immortal ownership); W11
  gets name/default_value/client_ty_idx without copies.

- **W1 DONE 2026-07-16** (lead-verified: 20 test cases / 115 assertions / ctest 1/1).
  AbiModel.h header-only PODs (~32 kinds, variant-based Ty, recursive ConstExpr via
  unique_ptr, RefInt256 bigints); AbiLoader fail-closed with FULL invariant list (14
  reject units + accept on all fixtures); fixtures → all 26 (18 ABIs; imports/ ×7).
  **PLAN CORRECTIONS from TDD evidence**: (1) struct_instantiations[].struct_name is
  MANGLED ("ResetTo<int64>") — strip <...> before decl lookup (abi-types.ts doc comment
  elides this); StructRef.struct_name IS plain. (2) prefix_len≤32 bound WRONG for
  struct-level prefixes — 48-bit exists (MsgSinglePrefix48); struct bound now ≤1023 +
  uint64 prefix_num; union-variant ≤32 stands (verified across all 26 fixtures).
  (3) err-*.tolk compile to VALID ABIs ("err" = runtime/codegen-time, not compile) —
  committed as future W11 reject-testdata. (4) small.tolk doesn't compile at all
  (upstream tolk bug, unrelated) — source committed, no ABI, excluded from loads-all.
  (5) dup-decl-name check is PER-KIND (reference SymTable = 3 separate maps, cross-kind
  reuse legal). CHECK-macro guard needed in smoke_test.cpp too (any TU pulling
  Status.h→logging.h→check.h). Executor self-caught a reinterpret_cast-across-types UB
  before it ever built (replaced with function template).

- **W0 DONE 2026-07-16** (lead-verified: build + ctest 1/1, VectorRunner 12 skipped/0
  enabled = green-empty confirmed, regen byte-stable across consecutive runs). Landed:
  ton-abi/ scaffold (lib + ton-abi-test + ctest, doctest v2.4.11 vendored, root
  add_subdirectory), tools/gen_vectors.mjs with EXACT pins (@ton/tolk-abi-to-typescript
  0.5.0 published, @ton/tolk-js 1.4.1, @ton/core 0.62.0 = reference's own devDep lock),
  3 fixtures compiled (counter, lots-of-messages, lots-of-wrappers scalar structs), 12
  vectors all enabled_from W8, self-validation (a)(b) pass ((c) vacuous — scalar-only
  choice, custom impls deferred to W8/W9), VALUE_SCHEMA.md (validated subset).
  FINDINGS: (1) td check.h redefines CHECK → shadows doctest's; push_macro/undef/
  pop_macro pattern REQUIRED in any TU mixing doctest + tdutils (in VectorRunner.cpp,
  remember for all future test TUs). (2) convertTolkFileToABI not in published exports
  — gen_vectors carries its own ~10-line wrapper. (3) §0.14 BOC-flags item NOT
  satisfiable at W0 (no cell-typed field in chosen vectors) — OPEN, moves to W5 (first
  cell vector); recorded in VALUE_SCHEMA.md. (4) ctest runs scoped to build-dir/ton-abi
  (root has no enable_testing — same as mch pattern). Note: T0.3 uses doctest SUBCASE
  per vector (no dynamic TEST_CASE registration in doctest) — same granularity.
