# MCH C++ host/runtime architecture findings

**Date:** 2026-07-12  
**Status:** exploratory; no redesign decision has been made

## Purpose

This report identifies practical ways to make the C++ MCH engine easier to extend, especially when a matcher needs a host function, a lookup, or generated build code.

The intended reader is an engineer choosing the next MCH architecture/refactoring slice. After reading, they should be able to select a candidate, understand its scope and trade-offs, and turn it into a focused design or implementation plan.

The review covers the matcher runtime, build runtime, host bindings, lookup integration, code generation, classifier API, production actor, and Python differential-test twins. Message parser implementations are intentionally out of scope, except where their interface affects host/runtime design.

## Executive assessment

The matcher engine itself is not the main source of friction. The walker, generated build functions, slot-indexed captures, artifact identity checks, deterministic matcher ordering, and Python/C++ differential gates form a coherent implementation. They should not be remade without new correctness or profiling evidence.

The largest remaining friction sits around the host/runtime seam:

- host functions receive untyped `Value` arrays and repeat validation and conversion;
- protocol implementations navigate mutable block representation directly;
- several DEX hosts independently construct the same action vocabulary;
- the production lookup interface is synchronous even though tier-2 lookup is naturally asynchronous;
- the classifier interface exposes preparation objects, arenas, and pointer lifetimes;
- code generation consumes raw IR separately from the runtime loader and emits several manually joined tables.

The recommended first refactor is a typed host authoring module, piloted on the shared DEX output model. The production lookup-wave interface should be designed before tier-2 storage is wired, because the current synchronous seam will otherwise harden into the actor integration.

## Current architecture in brief

The current execution path is:

1. Load and validate the versioned IR artifact.
2. Pair it with generated `where` and build callbacks using artifact identity.
3. Convert a trace into the event tree and basic block graph.
4. Run matchers in deterministic order, walking candidate blocks and collecting captures.
5. Execute the generated build program, including lookup collection/resume waves and host calls.
6. Merge accepted matches into produced blocks.
7. Run post-processing and adapt buffered core actions to a dump, shadow summary, or future persisted action.

Important recent improvements are already present: reproducible artifact emission, the live C++ surface dump for twins, CTest wiring, atomic classification results, structured failure categories, production schema loading, a production interface lookup source, and an anchor-prefilter change in the current working tree. These should be treated as existing foundations rather than re-proposed work.

## Recurring host-function patterns

The host files contain legitimate protocol-specific algorithms, but much of their size is representation plumbing. The same patterns recur across families.

### Untyped argument decoding

Every host call receives a build environment and a vector of generic values. Implementations repeatedly check argument count, inspect `Value` tags, convert individual elements to blocks, decide whether null means rejection or a null result, and construct fault strings.

This is a shallow interface: callers and implementations must both understand the dynamic calling convention, while very little behavior is hidden behind it.

### Direct block-representation access

Host functions read public block fields such as next blocks, children, previous block, event nodes, opcode, type, and the data dictionary. They repeatedly implement searches such as:

- first child call with a particular opcode;
- all consumed blocks of a particular type;
- the message or body belonging to a block;
- a chain of previous blocks;
- stable ordering by logical time;
- extraction of a named data field.

This makes the host surface depend on the current block representation and spreads topology assumptions across protocol files.

### Repeated body decoding and lookup access

Several hosts deserialize the same message body or resolve the same wallet/interface data during one classification. The build environment caches declarative `parse` results, but imperative host parsing commonly bypasses that cache. Lookup calls also use string kinds and generic argument vectors directly.

### Repeated DEX action assembly

Coffee, DeDust, Stonfi, and Tonco independently assemble the same concepts:

- asset/amount legs;
- incoming and outgoing transfers;
- source and destination accounts and jetton wallets;
- peer-swap hops;
- source and destination assets;
- sender, failure, referral, and minimum-output fields.

Each implementation knows the output field strings and nullable conventions. One implementation mutates nested generic values after construction, including mutation through a `const_cast`. That is a strong signal that `Value` is an appropriate runtime interchange type but a poor authoring interface for domain records.

### Manual host catalog joining

Host declarations, implementations, registry rows, dynamic argument behavior, referenced capabilities, and documentation are separate. The live surface dump removed duplication in the Python twins, but adding a binding still requires maintaining multiple C++ locations and relies on convention for its argument and effect contract.

## Candidate 1: typed host authoring module

**Recommendation:** Strong  
**Suggested first slice:** argument adapters plus one converted host

Introduce a host authoring module that keeps the existing runtime ABI internally but presents typed interfaces to protocol code.

A target implementation could resemble:

```cpp
HostResult<SwapRecord> coffee_swap(
    HostContext& context,
    ConsumedBlocks consumed);
```

The adapter at the runtime seam would be responsible for:

- checking arity;
- decoding `Value` arguments into declared types;
- applying the function's null policy;
- converting typed results back to `Value`;
- attaching the binding name to structured diagnostics.

The host catalog should describe each binding once:

- public name;
- typed argument list;
- result type;
- required capabilities, such as lookup, body decoding, or topology navigation;
- purity/effect classification;
- implementation function.

From that catalog the implementation can derive dynamic dispatch, surface reporting, load-time validation, and eventually generated declarations or documentation.

This creates depth by hiding the dynamic ABI behind a small typed interface. Tests can target protocol functions using typed inputs, while separate adapter tests pin generic `Value` behavior.

## Candidate 2: shared DEX record module

**Recommendation:** Strong  
**Suggested first slice:** Coffee, followed by DeDust/Stonfi/Tonco

Add typed records for the stable action vocabulary shared by swap protocols. A minimal model would include asset/amount, transfer leg, swap hop, and swap record types. One encoder would own the exact output field names and nullable-value convention.

Protocol hosts would then derive facts rather than assemble generic dictionaries. For example, a Tonco implementation would determine its incoming leg, outgoing leg, hops, failure state, and target asset, then return a `SwapRecord`.

Benefits:

- action-schema knowledge gains locality;
- field-name drift becomes much harder;
- invalid partial records can be rejected before encoding;
- nested records can be updated normally instead of mutating generic `Value` storage;
- tests can compare typed protocol results independently of runtime serialization;
- a future action-schema generator has one clear target interface.

The deletion test supports this module: deleting it would redistribute the same record construction and validation across at least four protocol files. It therefore earns its interface.

## Candidate 3: read-only block and match views

**Recommendation:** Strong, introduced incrementally  
**Suggested first slice:** helpers already duplicated by at least two hosts

Introduce narrow read-only views over blocks and the consumed match set. Likely operations include:

```cpp
block.field("amount");
block.message();
block.body();
block.first_child_call(opcode);
block.children_of_type("jetton_transfer");
match.consumed().calls(opcode);
match.consumed().by_type(type);
```

The view should expose the stable host contract, not every underlying field. It is also the natural owner of trace-scoped body decoding and message-parse caching.

This seam protects host code from block representation changes and concentrates ordering, null, and traversal rules. It also provides a place to forbid access to legacy Python-only block internals.

Avoid building a generic graph-query language up front. Add an operation only when multiple hosts already demonstrate the same requirement. One adapter with no second use would be a hypothetical seam; repeated Coffee/DeDust/Stonfi/Tonco patterns make the initial operations real.

## Candidate 4: lookup-wave classification API

**Recommendation:** Strong before tier-2 production wiring  
**Suggested first slice:** design and a synchronous compatibility adapter

The lookup source currently exposes a synchronous single-request function. That works for fixture data and in-block interface maps. It does not naturally represent a database, kvrocks, or actor request that must suspend classification.

The engine already implements lookup discovery and dependent lookup waves internally. Promote that behavior to an explicit resumable interface:

```cpp
auto run = engine.begin(trace);

while (run.has_pending_lookups()) {
  auto results = co_await provider.fetch_batch(run.pending_lookups());
  run.resume(results);
}

Classification result = run.finish();
```

A lookup request should have typed identity rather than a concatenated string key. A lookup result should distinguish:

- found value;
- semantic miss;
- malformed request;
- infrastructure failure.

The current fixture and in-block sources can be adapters that answer a wave immediately. A production adapter can batch and await requests without putting storage or actor logic inside the pure matcher module.

This is a larger change than adding `fetch_batch` alone. A synchronous batch function still cannot suspend the classifier and would leave the production integration problem unresolved.

## Candidate 5: owning classifier interface

**Recommendation:** Worth exploring before action persistence  
**Suggested first slice:** façade over existing preparation and classification

The current core classifier interface exposes the loaded IR, prepared callback tables, trace context, lookup source, and raw block pointers whose lifetime is tied to the caller's arena. This is efficient internally but wide for production consumers.

Introduce an owning `MchEngine` module that loads and validates an artifact once and exposes a smaller classification interface. It can support two result forms:

- a borrowed in-place result for internal high-performance adapters;
- an owned action draft for persistence or external consumers.

The command-line harness and production actor are already two distinct adapters, so this is a real seam. The new module should initially delegate to the existing implementation rather than rewrite it.

## Candidate 6: unified compiled-artifact code generation

**Recommendation:** Worth exploring after host and lookup interfaces stabilize  
**Suggested first slice:** shared typed IR model and validation

The generator currently reads raw JSON independently for expression vectors, `where` callbacks, and build callbacks. The runtime loader separately interprets and validates the artifact, and runtime preparation joins generated tables to loaded matchers.

The deeper target is one validated typed IR model feeding both runtime preparation and code generation. For a production artifact, one generator invocation could emit a compiled-artifact module containing:

- generated `where` and build callbacks;
- callback tables indexed by matcher/node identity;
- resolved binding requirements;
- artifact metadata and compatibility identity;
- a single manifest consumed by engine preparation.

The interpreted path should remain for conformance tests and arbitrary testbed artifacts. Production does not require hot reload, so a compiled module is compatible with the current deployment model.

This redesign can reduce manual joining and make generated code easier to inspect, but it should not precede the host catalog and lookup-wave decisions. Otherwise codegen will permanently encode interfaces that are already known to be too wide.

## Immediate authoring workflow improvement

Before a codegen redesign, add one supported authoring command that performs the full contract check:

```text
mch sync --check --cpp-engine <engine>
```

The command should:

1. compile the committed matcher sources through the manifest;
2. compare or update the committed IR artifact;
3. query the live C++ registry surface;
4. report missing parser, predicate, host-function, shaper, and lookup bindings;
5. rebuild affected generated tables;
6. run the relevant conformance and differential gates.

Artifact emission is now reproducible and the live surface already exists, so this is mostly orchestration. It would improve matcher authoring sooner and with less semantic risk than remaking the generator.

## Recommended implementation order

1. Define typed DEX records and their single `Value` encoder.
2. Add typed host argument adapters and a minimal `HostContext`.
3. Convert Coffee as the pilot and lock its behavior with existing differential gates plus typed unit tests.
4. Convert DeDust, Stonfi, and Tonco, extracting only repeatedly demonstrated block/match view operations.
5. Consolidate binding metadata into the host catalog.
6. Design the resumable lookup-wave interface before implementing the tier-2 production adapter.
7. Add the owning classifier façade and owned action-draft result needed by persistence.
8. Reassess unified compiled-artifact codegen once the new interfaces are stable.

## Areas not recommended for redesign now

### Matcher walker

The walker encodes subtle parity requirements: savepoint rollback, child accumulation, ordered consuming children sets, recursive frontier semantics, mutation visibility, and produced-block composition. Existing differential tests give it a strong correctness bar. Refactor only around a demonstrated defect or a profile.

### Generic `Value` representation

`Value` is useful as the language-neutral runtime interchange type and supports arbitrary-width TON amounts. The problem is using it as the protocol-authoring interface, not necessarily its internal representation. Typed records and adapters should be attempted before changing `Value` itself.

### Matcher ordering and generated build functions

Priority and mutation visibility are observable semantics. Generated build statements, slot-indexed captures, artifact identity guards, and the Python runtime as semantic oracle are sound decisions worth retaining.

### Broad optimization work

Current measurements do not make expression evaluation, integer allocation, or generated translation-unit size a production blocker. The anchor prefilter already addresses the most obvious scaling issue. Further optimization should be driven by shadow-mode profiles.

## Top recommendation

Start with the shared DEX record module and typed host adapter, using Coffee as the pilot.

This slice has the best ratio of leverage to risk. It removes unsafe generic-value manipulation, centralizes a schema already duplicated across several protocols, creates a testable host-authoring interface, and does not require changing matcher semantics, IR format, or code generation. The result will also provide concrete evidence for the eventual host catalog and generated typed bindings.
