# MCH C++ Engine: Fresh Architecture Review

**Date:** 2026-07-11

**Scope:** `ton-index-worker` C++ MCH runtime, message parsing, matcher execution, build programs, host bindings, IR loading, code generation, graph mutation, and verification tooling.

**Snapshot:** current working tree, including uncommitted MCH work present on the review date.

## 1. Purpose and intended reader

This document is for an engineer deciding what to fix or redesign next in the C++ MCH implementation.

After reading it, the engineer should be able to:

- distinguish confirmed correctness defects from architectural improvements;
- select an independent workstream;
- understand the compatibility constraints around that workstream;
- turn the recommendation into an implementation plan without rediscovering the current architecture.

This is a fresh review of the implementation rather than a restatement of the existing roadmap. The previous MCH progress memory and design documents were used as context, but their conclusions were rechecked against the current code.

## 2. Executive assessment

The C++ implementation has a strong semantic foundation:

- generated expressions and interpreted expressions share the same runtime leaves;
- slot-indexed captures avoid name-map overhead in generated build programs;
- source hashes prevent silently pairing generated functions with the wrong artifact;
- arena ownership gives blocks stable addresses;
- Python/C++ differential harnesses have already exposed real semantic defects in both implementations;
- many Python quirks are documented and deliberately reproduced instead of being accidentally approximated.

The main problem is no longer whether the walker or expression semantics can be ported. They can. The problem is that the engine is becoming a collection of parallel mechanisms that callers must join correctly:

- runtime IR metadata plus generated build tables plus generated `where` tables;
- global predicate/function/shaper maps plus a separate parser map;
- fixture lookup capabilities used as if they were engine capabilities;
- graph mutation, action rendering, fixture access, fallback behavior, and classification in one execution path;
- several independently maintained lists describing the same message or host surface.

The highest-leverage direction is therefore consolidation: compile and validate one execution plan, inject one binding catalog, make classification atomic, and deepen the parsing module around generated descriptors.

Before that redesign, four correctness issues should be fixed immediately.

## 3. Priority model

- **P0 — correctness now:** can produce an incorrect result, silently omit supported matchers, or make validation unreliable.
- **P1 — production blocker:** likely to cause incorrect failure behavior, integration difficulty, or serious maintenance risk when embedded in `ton-index-worker`.
- **P2 — leverage/performance:** meaningful simplification, testability, or hot-path improvement after correctness is secure.
- **P3 — hygiene:** documentation, diagnostics, or small test-harness issues.

## 4. Immediate correctness findings

### 4.1 P0: lookup request keys can collide

The two-phase lookup cache identifies a request by concatenating the lookup kind and argument payloads with NUL separators. The encoding has neither type tags nor length framing.

Examples of collisions include:

- one string argument containing an embedded NUL versus two string arguments;
- a string, byte sequence, and account carrying the same textual payload;
- ambiguous fallback renderings for values not handled explicitly.

Both the collecting table and the filled table trust this key. A fetched value for one logical request can therefore be returned for another request.

**Recommendation:** introduce a typed `LookupRequest` value with structural equality and hashing. If a byte encoding is still needed, make it type-tagged and length-prefixed. Add collision vectors covering embedded NULs, different arities, and equal payloads of different types.

### 4.2 P0: `Float` and `Amount(float)` semantics are incomplete

The recently added float representation is usable for rendering but not as a coherent expression value:

- an `Amount` is treated as `Amount(None)` whenever its integer field is null, even when it actually carries a float;
- two different float-backed amounts can therefore compare like two `Amount(None)` values;
- the `.value` accessor returns null for a float-backed amount;
- structural conformance equality has no `Float` case;
- amount structural equality examines only the integer field.

The current NFT paths may mostly render these values, but the general runtime interface is unsound.

**Recommendation:** represent `Amount` with an explicit payload tag: `None`, integer, or float. Define float equality and ordering deliberately, or reject floats at ingestion if the MCH language should not support them. Add expression and build vectors before using floats in more matchers.

### 4.3 P1: trace failure is not atomic

Classification prints each action immediately after a successful matcher build and mutates the block graph before the trace-level exception handler completes. If a later matcher, parser, shaper, or lookup wave throws, the engine prints `TRACE_FAIL`, but earlier action lines have already escaped and earlier graph mutations remain.

This contradicts the documented behavior that a failed trace is discarded and falls back as a whole.

Shapers make the problem more serious: they run after merge and mutate through raw block pointers. A throwing shaper can leave a partially changed graph.

**Recommendation:** classification should return a per-trace result buffer and commit it only after the full trace succeeds. Graph changes should be represented as a checked edit plan or applied to an isolated draft. CLI rendering must be an adapter over the committed result, not part of the engine implementation.

### 4.4 P0: the artifact references an unregistered message parser

Both declarative Stonfi V2 provide-liquidity matchers parse `StonfiV2ProvideLiquidity`, but the C++ parser registry does not contain that type. Build inclusion checks treat the missing parser as a hard skip, so the matchers cannot run end to end in C++.

The message layout is small and clean: opcode, query id, user address, and two coin amounts. The declarative matcher consumes the user and amount fields.

**Recommendation:** implement this parser and parity coverage immediately. Use it as the first pilot for descriptor-driven parser code generation. Add a build-time invariant requiring every message type referenced by an artifact to exist in the C++ parser catalog.

### 4.5 P0/P1: malformed event graphs are not defensively rejected

Trace-to-event-tree construction assumes each node has one parent and that the structure is acyclic, but it does not enforce either property:

- linking can overwrite an existing parent and append the child under multiple parents;
- root ascent has no cycle guard;
- reachability counting has no visited set;
- duplicate reachability can make orphan counts unreliable or underflow size arithmetic.

Normal fixtures may not trigger this, but malformed or duplicated message linkage can hang or corrupt classification.

**Recommendation:** validate unique parenthood during linking, detect cycles, and return a structured malformed-trace error. Use visited sets as defense in depth even after validation.

## 5. Parsing findings and proposed direction

### 5.1 P1: parser metadata is duplicated and stringly typed

The same logical parser surface is described in several places:

- a manual message-name-to-function map;
- a separate opcode-to-type table for parse dumps;
- function declarations in the internal parser header;
- CMake source lists;
- artifact message-type references;
- Python twin surface lists;
- implicit field names inside untyped `Value` objects.

There is no central field schema. A misspelled body field can survive compilation and fail only at runtime. Duplicate object fields are also possible, and field lookup silently returns the first occurrence.

**Recommendation:** define one generated or `constexpr` message descriptor containing:

- canonical name;
- canonical opcode where applicable;
- opcode validation policy;
- output field schema and optionality;
- parser callback;
- constructibility/test metadata;
- serialization policy for cells and payload bytes.

Generate the registry, parse-dump selection, surface report, artifact capability check, and parser test matrix from that descriptor catalog. Use an object builder that rejects duplicate fields and missing required fields.

### 5.2 P1: parsing is hidden mutable state

The current `parse target as A | B` statement mutates a body map in `BuildEnv`, keyed by block pointer. A later `.body` access depends on that earlier source-order side effect.

Consequences:

- parsing the same block again overwrites its previous parsed body;
- list parsing has implicit gap and null behavior;
- it is difficult to tell which alternative matched;
- generated lookup discovery reruns the parsing side effect on every dry pass;
- static field validation has no explicit parsed value to type.

**Recommendation:** in the next IR version, make parsing an expression binding, conceptually:

```mch
let msg = parse raw as TypeA | TypeB
require msg
out { amount: msg.amount }
```

The result should be a nullable typed value, optionally retaining the selected type. The existing statement syntax can remain as compatibility sugar lowered into the explicit form.

### 5.3 P1: raw captures and produced blocks need different interfaces

Parsing a raw message-bearing block and reparsing a block produced by an earlier matcher are not the same operation. A produced block can contain several merged event nodes, and choosing the first event node is not a stable semantic definition of its body.

The resolver now correctly rejects some parse-on-composed-capture cases. That restriction should become an explicit model rather than a special-case diagnostic.

**Recommendation:** distinguish `RawMessageCapture` from `ProducedBlockCapture` in the resolved model and IR:

- raw captures may be parsed;
- produced blocks expose a versioned `.data` contract;
- if a matcher genuinely needs the original raw body across composition, the producer carries an explicit body origin or source capture.

Do not silently change the current first-event-node behavior without a compatibility migration; it can change historical action data.

### 5.4 P1: soft parse failures erase useful information

Parser dispatch catches all exceptions and reduces them to a generic error. Soft parsing then collapses no body, type mismatch, malformed body, unsafe VM error, and unknown parser into null.

This is suitable as the final compatibility result but poor as an authoring and operational interface. It also risks classifying serious infrastructure or resource failures as ordinary matcher rejection.

**Recommendation:** add a checked `ParseCursor` and structured attempt result:

- `matched(value)`;
- `mismatch(reason, path, bit offset, ref offset)`;
- `malformed(reason, path, bit offset, ref offset)`;
- fatal/infrastructure error propagated to the trace failure interface.

Soft parse may still map ordinary mismatch and malformed results to null for parity, while recording diagnostics for authoring and observability.

The cursor should own checked operations for integers, coins, addresses, references, optional references, and remainders. Generated parsers and most hand-written parsers should not use unsafe raw cell operations directly.

### 5.5 P2: parsing should be generated from a logical message schema

Wrapping `tlbc` records is not enough. `tlbc` intentionally lowers several semantic TON types into slices, leaving MCH adapters to recover addresses, variable integers, optional payloads, and Python-specific quirks manually.

**Recommendation:** add a neutral MCH message schema describing logical output rather than only TL-B storage:

- field names and logical types;
- opcode policy;
- optionality;
- inline/reference payload policy;
- output serialization policy;
- named hand-written tail hooks for exceptional protocols.

Generate the checked C++ cursor adapter and descriptor metadata first. Clean message heads such as Stonfi V2 provide liquidity, jetton burn, internal transfer, and mint are good pilots. Keep hand-written hooks for pytoniq quirks and protocol-specific tails.

Generating Python adapters from the same schema is a later option, after differential fuzzing proves exception timing and alternative ordering.

### 5.6 P2: body decoding is repeated across modules

Build parsing, host functions, NFT helpers, leaf initialization, and TON comment extraction contain overlapping body decode/deserialization logic. Lookup discovery can rerun parts of it several times.

**Recommendation:** introduce a trace-scoped message-body cache keyed by message identity. Cache either the decoded cell or a structured decode error. Add a second cache keyed by message/type descriptor for parsed values.

This module should also own body-origin semantics, so every caller observes the same source cell.

### 5.7 P2: pytoniq BOC ordering remains a performance risk

The compatibility serializer reorders shared cell graphs by removing and revisiting cells and can repeatedly walk shared subtrees. Adversarial DAGs can amplify the work.

The existing design notes already question whether raw BOC container equality is a real database invariant or merely an implementation detail.

**Recommendation:** decide this separately from parser refactoring. Prefer cell identity/root-hash equality internally and native TON serialization at the persistence seam. If legacy byte identity must remain, bound traversal work and cache revisited subtrees. Any database-visible byte change needs a consumer audit and migration note.

### 5.8 P3: parse-dump can report success after fixture failure

Fixture load errors are printed, but the top-level parse-dump mode still returns success.

**Recommendation:** propagate failure status and include fixture, message type, and structured parse error in diagnostics.

## 6. Runtime and classification findings

### 6.1 P1: binding capabilities are global and fixture-coupled

Predicates, host functions, and shapers live in global static maps. Parser capability lives in a different global map. Lookup runnability is checked against the fixture lookup implementation rather than against the actual lookup adapter supplied to the engine.

This makes embedding and testing harder and allows advertised capabilities to drift from the implementation used at runtime.

**Recommendation:** inject one immutable binding catalog into plan compilation. It should contain:

- predicates;
- host functions;
- shapers;
- message descriptors;
- a lookup adapter that advertises supported lookup kinds and schemas.

Plan compilation resolves every referenced name once and produces either a runnable matcher or a structured missing-capability report.

### 6.2 P1: lookup discovery repeatedly executes the whole build program

The two-phase model reaches a lookup fixpoint by rerunning the generated build function with a fresh environment. This is semantically effective for lookup arguments that depend on previous lookup results, but it repeats parsing, expression evaluation, allocations, and host function calls.

Host functions receive mutable build state, while purity during discovery is only a convention. A future side-effecting host function could run several times before the final build pass.

**Recommendation:** retain the user-approved lookup-wave model, but deepen its implementation:

- represent discovery as `NeedLookups`, `Rejected`, or `Accepted`;
- make the discovery interface side-effect-free;
- cache parsing and pure intermediate results across waves;
- batch requests across all active candidates in a trace where possible;
- distinguish missing lookup values from lookup adapter failures.

A resumable generated implementation or compact statement interpreter could support this. A smaller intermediate step is to split host bindings into discovery-safe pure functions and final-only effects.

### 6.3 P1: classification is mixed with harness concerns

The current classification path owns fixture lookup construction, filesystem traversal, action string rendering, stdout/stderr behavior, skip reporting, graph mutation, and trace fallback.

This is a shallow module: its interface complexity is almost the same as its implementation complexity, and the production integration cannot reuse it without inheriting harness behavior.

**Recommendation:** create a deep classification module whose interface accepts a trace, validated execution plan, binding catalog, and lookup adapter and returns a structured classification result. CLI dumps, Python parity rendering, and `ton-index-worker` action persistence become adapters.

### 6.4 P1: generated and interpreted matcher pieces are joined manually

Matcher metadata comes from the runtime loader. Build functions are found by artifact array index in a SHA-selected generated table. Generated `where` functions live in another table, while classification currently uses the interpreted where path.

The SHA guards correctly prevent silent mismatch, but callers still have to assemble the executable matcher from parallel structures.

**Recommendation:** code generation and loading should produce one execution-plan record per matcher containing:

- stable matcher identity;
- compiled match graph;
- generated `where` callbacks;
- generated build callback;
- resolved host/parser/lookup bindings;
- priority and anchor metadata.

The interface to classification should not expose artifact indexes or table-selection logic.

### 6.5 P2: anchor indexes are built but not used

The loader constructs opcode and block-type indexes. Match-only execution still scans every matcher for every block, while classification performs a full breadth-first traversal for each matcher.

Matcher-first traversal may remain necessary because earlier matchers mutate the graph and later matchers observe produced blocks. Even so, candidate anchors can be prefiltered by trace opcode/block-type inventories, and impossible matchers can skip traversal entirely.

**Recommendation:** preserve matcher order, but use the plan's anchor metadata to prefilter candidate blocks. Predicate anchors remain full-scan unless a safe secondary index exists.

### 6.6 P2: walker canonicalization copies and sorts repeatedly

The block substrate claims that sibling lists remain in canonical order, yet the walker repeatedly copies and stable-sorts them. Recursive and backtracking patterns multiply these allocations.

**Recommendation:** enforce canonical order at every graph mutation and iterate spans directly. Use one scratch buffer only for concatenated recursion frontiers where order cannot be inherited.

### 6.7 P3: implementation comments have drifted

Some public comments still say named predicates are not evaluated and shapers have no engine call site, while the current implementation does both. Duplicate block-key rendering also remains.

**Recommendation:** update comments during the corresponding refactor and keep diagnostic formatting in a harness module rather than runtime modules.

## 7. Block graph findings

### 7.1 P1/P2: topology invariants leak through public fields

`Block` exposes mutable relationships for next, previous, parent, and consumed children. Classifiers and shapers mutate them through raw pointers. Correctness depends on several representations staying synchronized and canonically ordered.

**Recommendation:** deepen this into a `BlockGraph` module with checked operations:

- connect and disconnect;
- merge plan and commit;
- insert-between;
- shape edit plan;
- canonical iteration;
- stable block handles;
- debug invariant verification.

Arena-backed pointers can remain an implementation detail if their lifetime is useful. The important change is to stop exposing the invariant-bearing topology as unrestricted mutable state.

### 7.2 P1: shapers need a result and failure contract

Shapers currently return `void` and mutate the graph directly. They cannot report an expected rejection separately from an internal fault, and they cannot participate in transaction rollback.

**Recommendation:** shapers should return a checked shape result or graph edit plan. Classification validates and commits the plan only after the matcher build and merge are known to be valid.

## 8. IR loading and code-generation findings

### 8.1 P0/P1: loader and generator do not share a complete validator

The generator decodes generic JSON and accesses expected fields directly. Several malformed shapes can throw or assert. Some missing strings become empty strings, and a missing/non-array build program can lower into an accepted empty build.

The runtime loader validates a different subset. Unknown node kinds can degrade to a permissive kind, missing roots and registration indexes are insufficiently checked, and some numeric parsing can throw outside structured diagnostics.

**Recommendation:** extract one typed IR module used by both loader and generator:

```text
JSON bytes
  -> schema/version check
  -> typed Artifact / Matcher / Node / Expression / Statement
  -> complete invariants and source-path diagnostics
  -> interpreter or generated execution plan
```

Unknown kinds, missing roots, invalid references, duplicate captures, bad registration order, unsupported schema features, and malformed statements must fail closed.

### 8.2 P1: generated artifact selection is hardcoded

The public generated interface names contract, vector, and optional testbed tables separately. The optional table requires a handwritten empty implementation, and selection logic manually compares hashes.

**Recommendation:** generate an artifact manifest such as:

```text
GeneratedArtifact
  schema version
  codegen ABI
  source digest
  semantic digest
  build callbacks
  where callbacks
  capability requirements
```

Expose a catalog of manifests and select by identity. Adding another artifact should be data and build configuration, not a new public function family.

### 8.3 P1: one artifact is read and generated in separate invocations

The contract artifact is independently read for build and `where` generation. Concurrent edits can theoretically produce outputs from different file versions. Testbed generation also covers build functions but not generated `where` functions.

**Recommendation:** provide one `compile-artifact` invocation that validates and hashes once, then atomically emits the manifest and all implementation shards.

### 8.4 P1/P2: generated output is monolithic and always rewritten

The main build-program output is a large serial translation unit. A small artifact edit rewrites and recompiles the entire file. The generator rewrites outputs even when the bytes are unchanged.

**Recommendation:** emit a fixed number of deterministic shards plus a small manifest translation unit. Write to a temporary file, replace atomically, and preserve the existing file when content is byte-identical. Fixed bucketing avoids dynamic build-system discovery while allowing parallel and incremental compilation.

### 8.5 P2: generated hot-path code retains dynamic dispatch

The generator knows builtin names, accessor names, capture slots, argument counts, field counts, and parse alternatives statically, yet emitted code still performs string dispatch, constructs temporary argument vectors, copies large `Value` objects, and repeatedly constructs parse-type vectors.

**Recommendation, after profiling:** emit direct builtin/accessor calls, use references for capture reads, reserve/move known field collections, and emit static parse descriptor arrays. Apply these in the generator so all matchers benefit and verify them through the existing differential gates.

### 8.6 P1: compatibility identity is raw-file SHA only

Whitespace or JSON key-order changes alter the source hash even when semantics are identical. The generator does not currently declare its own ABI or feature set.

**Recommendation:** keep raw SHA for provenance, but pair execution using schema version, codegen ABI, supported feature flags, and a canonical semantic digest. Error messages should distinguish source mismatch, schema incompatibility, and generator ABI incompatibility.

### 8.7 P1: generated output escaping is incomplete

The C++ string literal emitter handles common escapes but can emit other control bytes unsafely.

**Recommendation:** validate a strict UTF-8 policy or emit robust byte escapes with correct hexadecimal adjacency handling. Cover all JSON control characters in generator tests.

### 8.8 P2: contract synchronization remains manual

The C++ artifact copy can drift from the Python-produced source of truth.

**Recommendation:** add an explicit synchronization/verification command and CI gate. Python does not need to become a normal C++ build dependency; CI can regenerate into a temporary location and compare semantic digests.

## 9. Host binding findings

### 9.1 P1: host escapes create dual-implementation tax

Every host function carries Python semantics into an imperative C++ implementation and usually requires separate surface-list maintenance. Large host data builders can bypass the benefits of declarative build programs.

**Recommendation:** keep host functions for irreducible protocol logic, but prefer small typed domain primitives over action-sized builders. Resolve and type-check their arguments during plan compilation. Require an explicit purity/effect classification so lookup discovery cannot accidentally rerun effects.

### 9.2 P1: host functions can read representation-specific block internals

Some Python host functions depend on legacy subclass attributes not represented by generic declarative blocks. This caused declarative matchers to reject or silently omit data.

**Recommendation:** host functions may read only the stable block envelope and versioned `.data` contract unless they declare a legacy-only capability. Add an authoring-time check for host functions used by declarative matchers.

### 9.3 P2: computed record lookups do not fit the basic fixture adapter

The JVault asset lookup performs multiple dependent interface reads and cell parsing inside one fixture lookup branch. It works, but the lookup adapter now contains protocol implementation logic.

**Recommendation:** model complex lookups as named typed resolvers behind the lookup seam. Their production and fixture adapters should share the same result schema and failure contract. Do not force every resolver into a flat kind-to-map abstraction.

## 10. Testing and observability findings

### 10.1 P1/P2: no native C++ tests are registered

The executable exposes valuable vector and parity modes, but the MCH CMake module registers no native tests. Verification depends on manually running command sequences and Python twins.

**Recommendation:** add CTest entries for fast deterministic checks while retaining full differential gates for integration:

- expression vectors, interpreted and generated;
- build vectors;
- typed IR negative corpus;
- generated artifact manifest pairing;
- lookup-key identity vectors;
- amount/float semantics;
- graph invariants and rollback;
- parser descriptor surface completeness;
- parser malformed-input corpus;
- deterministic/content-stable code generation.

### 10.2 P1: parser coverage does not match the parser surface

Current build vectors mention only a subset of message types referenced by the artifact. Parse-dump omits some registered types and cannot exercise alternative-parser ordering or deliberately unchecked opcode behavior.

**Recommendation:** generate tests per descriptor:

- minimal and maximal valid values;
- every truncation boundary;
- none, standard, external, variable, and anycast addresses;
- inline and referenced tails;
- malformed and excessive reference structures;
- high-bit unsigned integers;
- shared/deep BOCs;
- first-success ordering for alternatives;
- wrong-opcode behavior according to descriptor policy.

Every artifact-referenced message type should have at least one positive and one negative case.

### 10.3 P2: parsers and IR loader need fuzzing

Both modules consume attacker-influenced structured input and contain code paths that may throw or invoke VM assertions.

**Recommendation:** add subprocess crash corpora immediately and coverage-guided fuzzing when practical. The required invariant is stronger than “returns an error”: malformed input must never abort, hang, or allocate without bounds.

### 10.4 P1/P2: failures need structured categories

Skip reasons, clean rejections, malformed traces, parser mismatches, lookup absence, lookup infrastructure failure, codegen incompatibility, and engine faults are currently represented through a mixture of strings, nulls, exceptions, and stdout lines.

**Recommendation:** define structured outcome categories at module interfaces and render them only in adapters. Production observability should count them separately without changing parity output.

## 11. Recommended execution sequence

### Wave 0 — correctness and regression locks

1. Replace lookup key serialization with typed request identity.
2. Correct or remove float-backed amount semantics and add vectors.
3. Buffer classification output so `TRACE_FAIL` cannot coexist with committed actions.
4. Add `StonfiV2ProvideLiquidity` parsing and artifact/parser surface validation.
5. Add graph cycle and unique-parent validation.

These changes are narrow and should precede larger interface work.

### Wave 1 — typed IR and execution plan

1. Extract the shared typed IR validator.
2. Make both runtime interpretation and code generation consume it.
3. Generate an artifact manifest containing build and `where` callbacks.
4. Resolve all bindings once into an immutable execution plan.
5. Keep current artifact SHA checks during migration.

This is the top architectural recommendation because it removes the widest distributed interface.

### Wave 2 — parser descriptors and diagnostics

1. Introduce message descriptors and the checked cursor.
2. Generate registry, dump metadata, surface checks, and tests from descriptors.
3. Pilot logical-schema generation on the missing Stonfi parser and clean jetton heads.
4. Add trace-scoped body/parse caches.
5. Design explicit parse-expression binding for the next IR version.

### Wave 3 — atomic classification and graph seam

1. Extract classification from CLI and fixture code.
2. Return buffered structured results.
3. Encapsulate graph mutation and add checked edit plans.
4. Give shapers explicit result/error contracts.
5. Add the production lookup and persistence adapters.

### Wave 4 — lookup execution depth

1. Inject capabilities through the binding catalog.
2. Separate missing values from infrastructure failures.
3. Make lookup discovery side-effect-free.
4. Cache intermediate parsing/evaluation across waves.
5. Batch requests across candidates where dependency ordering allows it.

### Wave 5 — measured codegen and walker optimization

1. Establish production-like benchmarks.
2. Add deterministic output shards and content-stable writes.
3. Lower static builtin/accessor calls directly.
4. Remove unnecessary `Value` copies and temporary vectors.
5. Use anchor indexes and avoid redundant walker sorting.

Do not optimize generated code before the execution-plan and binding interfaces stabilize; otherwise the emitter will encode another temporary architecture.

## 12. Decisions and invariants to retain

The review does not recommend reopening these decisions without new evidence:

- generated build statements instead of a production expression VM;
- no hot reload in the production C++ engine;
- slot-indexed capture environments;
- shared runtime leaf semantics between interpreted and generated expressions;
- per-artifact identity guards that prevent silent mismatches;
- matcher priority and mutation visibility semantics required for Python parity;
- guarded recursion depth;
- stable arena ownership for blocks;
- two-phase/wave-based lookup semantics for dependent asynchronous lookups;
- explicit reproduction of Python quirks until a versioned migration deliberately removes them;
- differential Python/C++ testing as the final semantic gate.

The goal is to place these invariants behind deeper modules, not to weaken them.

## 13. Suggested workstream boundaries

The following workstreams can be planned largely independently after Wave 0:

1. **Typed IR and artifact manifest** — loader, validator, generator, schema/versioning, negative tests.
2. **Message descriptor system** — checked cursor, parser metadata, registry generation, parser vectors.
3. **Atomic classification** — result buffering, CLI adapter extraction, failure categories.
4. **Block graph module** — checked topology operations, transactions, shaper edit plans.
5. **Binding and lookup catalog** — injected capabilities, typed requests/outcomes, production adapter shape.
6. **Verification infrastructure** — CTest registration, generated surface invariants, fuzz/crash corpora.
7. **Performance pass** — generated shards, direct lowering, copy reduction, anchor-index use, walker scratch storage.

The typed IR/execution-plan workstream should lead because the parsing, binding, and codegen improvements all benefit from one validated artifact model.

## 14. Final recommendation

Fix the confirmed defects first, then compile one validated execution plan as the next architectural milestone.

That plan should become the central interface between authoring artifacts and classification. It should own validated matcher metadata, generated callbacks, resolved bindings, capability requirements, schema identity, and anchor indexes. Parsing, lookups, graph mutation, CLI parity output, and production persistence should sit behind explicit seams with real production and test adapters.

This direction preserves the substantial parity work already completed while reducing the number of facts every future MCH contributor must keep synchronized manually.
