# MCH: How Everything Works and Why

An orientation document. The normative sources are `mch-spec.md` (language),
`mch-ir-spec.md` (artifact format), `mch-engine-v2.md` (walk semantics),
`mch-cpp-plan.md` (implementation phases + gates), `mch-cpp-buildprog-design.md`
(build-execution design), `mch-roadmap.md` (task ledger). This document explains
the shape of the whole thing and the reasoning behind the decisions — read it
first, then the specs.

---

## 1. The problem

The TON event classifier turns blockchain traces (trees of transactions/messages)
into structured `Action` rows. Historically every protocol was classified by a
hand-written Python `BlockMatcher` — an imperative pattern matcher composed of
`ContractMatcher` / `child_matcher` / `RecursiveMatcher` objects plus a
`build_block()` method that assembles the result. ~30+ protocols, each a few
hundred lines of Python, each subtly different in style, none portable.

Production needs the classifier in C++ (the indexer worker). Porting 30 hand
matchers by hand — and then keeping two divergent codebases in sync forever —
was the non-starter that motivated MCH.

**MCH** (`.mch` files) is a declarative matcher DSL: one spec file describes both
the *pattern* (what shape of message tree to find) and the *build* (what data to
produce from it). A single Python frontend compiles specs into a
language-neutral IR artifact; any runtime that implements the IR semantics can
classify identically. Write once, run in Python and C++.

## 2. The pipeline

```
 .mch specs ──(Python frontend: lexer/parser/resolver/compiler)──▶ IR artifact (JSON)
                                                                        │
                     version-controlled contract (ir/ dir)              │
                                                                        ▼
                  Python IR engine (indexer/events/mch_ir/)   ← reference runtime
                  C++ engine (ton-index-worker/ton-mch-engine/) ← production-bound
```

- **Frontend stays Python forever.** It runs at development time only. Compiler
  diagnostics (C0xx/R0xx/P0xx/W0xx codes) live there; runtimes never compile.
- **The IR JSON is the contract.** It is committed (version-controlled), emitted
  by `python -m indexer.events.mch.ir_emit`, consumed by both engines. The C++
  build is completely Python-free: a C++ tool (`mch-codegen`) turns IR into
  generated C++ at build time, wired through CMake like tlbc.
- **The Python IR engine is ground truth.** Every C++ layer was accepted only
  after a byte-identical A/B against it (see §8).

## 3. The language in one page

A spec declares opcodes, predicates, rules (reusable patterns, possibly
recursive), and matchers. A matcher has:

- **anchor** (`entry`) — the single distinctive block that triggers a match
  attempt: an opcode, opcode set, btype, mixed set, or predicate.
- **pattern** — edges from the anchor: `->` child, `<-` parent, `{a; b}`
  children sets, `|` or / `^` exclusive-or alternatives, `maybe` optionality,
  `$rule` recursion (frontier or `cyclic` strategy), `@name` captures (scalar
  or list), `where` predicates / inline `where (expr)`.
- **build** — either a `build host_fn` directive (host builder), or a
  declarative program: `parse CAP as MsgType`, `let`, `lookup`, `reject when`,
  `failed/broken when`, `flow`, `out { field: expr }`, `produces switch`.
- **directives** — `produces` (union allowed), `priority` (default 100),
  `include_excess` / `include_bounces`, `shape` (post-merge host tree surgery).

The expression language is deliberately small: pure, total, no user-defined
functions, no loops. Field access, comparisons, `and/or/not`, `??`,
`x if c else y`, arithmetic `+ - *` (no division — nothing needed it yet),
list/record literals, a fixed builtin set (`account`, `amount`, `asset`,
`ton_asset`, `addr_none`, `b64`, `first`, `last`, `len`, `sum`, `zip`,
`map(xs, "field")`), and `.data` access into composed blocks. Every construct
has a one-line C++ implementation — that was an explicit acceptance criterion
for adding anything.

## 4. Decisions and why

The project accumulated a set of hard decisions. They are recorded where a
future reader will trip over them (spec §15b, roadmap "Language decisions"
block, plan §10), but here is the story:

**No cross-trace state, ever.** v0.1 had `stitch` (pending tables, TTLs) for
joining partial/final events across traces. Removed with prejudice — a trace is
processed in isolation, always. Cross-trace joining contradicts the domain
model and every runtime simplification flows from its absence. The keywords
stay reserved so the parser can emit a targeted migration error
(`P012_STITCH_REMOVED`).

**Intra-trace pairing stays host-side.** Dedust/stonfi partial+final deposit
merging is done by post-process passes (`post_process_dedust_liquidity`), not by
a language construct. Too rare and protocol-specific to buy language surface.

**No hot reload.** The C++ worker loads the IR once at startup or embeds it at
build time. No reload/versioning/invalidation machinery anywhere. This decision
unlocked immutable matcher tables and the codegen strategy.

**Generic block + dict is the canonical product (the "G11" decision).** A
declarative build produces `Block(btype, data: map<string, Value>)` — never a
Python subclass or dataclass. Decided by asking "what can C++ have?" instead of
"what does Python have?". Python-side strict tests were adapted to assert
btype+fields; the serializer got a dual-access helper (`_dget`) because the
legacy path still produces dataclasses.

**Host bindings are a budget, not a failure.** Named host functions —
predicates, builders, lookups, shapers, `fn` escape hatches, message parsers —
are the deliberate escape valve. Every one is a named parity liability that a
runtime must provide, so the language grew exactly until the *mechanical* parts
were expressible and stopped there. Gap verdicts (spec §15b): string/byte ops
CUT (host fn), multi-return lookups CUT (record-returning lookup pattern),
topology walks CUT (pattern-design + `msg.exit_code` envelope field),
`in` operator SKIPPED (or-chains cover constant sets), `map`-comprehension and
`any/all` folds DEFERRED evidence-gated (§16.1) — the swap family stays hybrid
regardless because multi-hop async pool resolution is irreducibly host-side.

**Determinism is normative.** Python's legacy engine inherited `set()` ordering
nondeterminism. The IR engine pins candidate order (ascending `min_lt`, stable)
at every choice point (T21) and matcher order as (priority, file, position)
(T20). C++ goes further: the substrate *maintains* canonical `next_blocks`
order as an invariant, so the walker reads lists raw. A C++ engine cannot
inherit Python hash order — this had to be spec, not accident.

**The compiled path died (T29).** There used to be a third runtime: the
compiler lowering specs directly to legacy `BlockMatcher` objects. Once the IR
engine passed the full corpus, the compiled path was demoted, then deleted
(~600 LOC). The compiler now lowers to plain `LNode` records whose only
consumer is the IR emitter. Byte-identical IR before/after the refactor was the
regression gate.

**Expressions are codegen'd, not interpreted (in C++).** Two reasons: no hot
reload means the artifact is known at build time; and generated straight-line
C++ beats a tree-walker. The safety rule that makes this sane is
**one-semantics**: all leaf semantics live in `ExprRuntime.{h,cpp}` as `rt_*`
free functions; the interpreter (kept for vectors/debugging) and generated code
call the *same* functions, so they cannot drift. The generator itself is a C++
tool (`mch-codegen`) so the C++ build has zero Python dependency.

**Build programs are also codegen'd (D2)** — one
`BuildOutcome build_<matcher>(BuildEnv&)` per matcher; `let` becomes a C++
local, `out` builds the dict, faults become clean rejections. No statement
interpreter exists in C++.

**Lookups are two-phase (D1).** Build programs are pure except lookups
(interface-repository reads: jetton wallets, NFT items...). C++ runs the
generated fn twice: a collect pass with a recording table (to fixpoint —
lookup-arg-of-lookup needs it), one batched fetch, then a resume pass with an
immutable filled table. No coroutines, deterministic, batches naturally per
trace.

**Environments are slot-indexed (D3).** Capture *names* exist only at compile
time; the walker binds slot numbers; `WhereEnv`/`BuildEnv` carry
`const Value *slots`. Cheap, and it keeps generated code free of string maps.

**BOC bytes are byte-identical cross-language (D4, user decision).** Cell-
derived strings land in DB-visible data (vesting's `message_boc_str`, jetton
`forward_payload`). The naive expectation "standard BOC serialization matches"
was FALSE: pytoniq's `Cell.order()` sinks shared subtrees to their last visit;
td orders differently. C++ therefore carries `pytoniq_to_boc()` (~100 LOC)
emulating pytoniq's writer, with CRC set per call site to match the exact
Python code path (payload bytes crc=true, `b64(Cell)` crc=false). If this ever
feels absurd, remember the alternative was blessing a permanent cross-language
byte divergence in DB rows.

**Generated tables carry their source identity.** `mch-codegen` bakes a sha256
of the input document into generated files; runners hard-fail (exit 2) when the
loaded artifact doesn't match. Added after a stale-table pairing produced a
convincing-but-wrong A/B failure.

## 5. How the engine works

Per trace:

1. **Tree build.** Transactions → `EventNode` tree via message-hash linkage
   (`to_tree`), then each node becomes a leaf `Block`: `ton_transfer`,
   `call_contract` (+opcode), or `tick_tock`, with exact failed-flag semantics
   ported from `basic_blocks.py` (bounce/uninit/no-gas exemptions; valueless
   log-call exemption). Contract deployments ride in `children_blocks`.

2. **Matching loop.** Matchers run in (priority, file, position) order. For
   each matcher, BFS over the block tree (only unconsumed blocks — the
   `parent is None` guard); at each block, the anchor test then `try_build`:

   - `match_node` walks the compiled pattern graph: kind test → `where` →
     bind (consume + capture slot) → edges in fixed order (child edge =
     multi-match accumulation over all `next_blocks`; children set = ordered
     first-match-wins; parent edge) → per-node excess/bounce absorption.
     Failure discipline: the caller rolls back to its savepoint
     (`MatchContext.mark/rollback`), so a failed sub-attempt leaves no residue.
   - Recursion: frontier strategy (step nodes advance a frontier; exit binds
     alignment-per-iteration; success iff ≥1 step landed) or cyclic descent
     (back-edges, for tonco-style loops). List captures get gap-aligned nulls
     per iteration (spec §7.2) so parallel lists stay index-aligned.
   - **Build phase.** Captures → slots; two-phase lookups; the build program
     runs. `reject when` true, any expression fault, or a failed merge ⇒
     **clean rejection**: nothing consumed, no side effects, the anchor stays
     available to later matchers (§10.7). Acceptance ⇒ a produced Block
     (btype + data dict) `merge_blocks` the consumed set into itself, then
     root-level excess/bounce blocks are auto-appended to the result.

3. **Post-processing.** Host passes (e.g. dedust liquidity pairing) run after
   matching. Then serialization to Actions (Python today; C++ Phase E).

A produced block is invisible to the matcher pass that made it but visible to
later matchers — that is what lets `ethena_deposit` anchor on a
`jetton_transfer` block that the jettons matcher produced moments earlier.
(Corollary: matcher *order* matters for composed anchors; with everything at
default priority 100 the tie-break is file name — a known trap flagged for
production wiring, see plan §10.)

## 6. The two runtimes

**Python IR engine** (`indexer/events/mch_ir/`): loader validates the artifact
(unknown host bindings are load errors), `build_matchers()` returns engine
objects the existing pipeline consumes. It replaced both the legacy hand
matchers for ported protocols *and* the deleted compiled path. It is the
semantics oracle.

**C++ engine** (`ton-index-worker/ton-mch-engine/`): same layers, built
bottom-up, each layer A/B-gated: trace loader (lz4+msgpack fixtures) → Block
substrate → IR loader + match walker → expression runtime (interpreter +
generated) → build runtime (generated) + two-phase lookups → classify loop.
Deliberate substrate deviations from Python are *documented in
`BlockTree.h`'s header* (canonical order invariant, frontier dedup, no
transient flag, orphan counting) — each chosen where C++ could be cleaner
without observable divergence.

The C++ tree also contains things Python doesn't need: `Value` (variant with
`td::RefInt256` integers — jetton amounts overflow int64; Python ints are
unbounded), `pytoniq_to_boc`, TL-B generated message parsers with
pytoniq-quirk adapters, and the `mch-codegen` tool.

## 7. Host bindings inventory

What a runtime must provide beyond the IR (the "budget"):

| Kind | Examples | C++ status |
|---|---|---|
| message parsers | JettonTransfer, ChangeDnsRecord, ... | Phase C, per-family TL-B + adapters |
| lookups | jetton_wallet, nft_item, nominator_pool | fixture-backed source done; production actor source later |
| predicates | pton_self_transfer, getgems_purchase, ... (~8) | Phase D |
| host fns | dedust legs/failed, tgbtc endian-hex, ... (~5) | Phase D |
| builders | remaining non-declarative matchers | Phase D/never (conversion preferred) |
| shapers | getgems proxy insert (1) | Phase D |
| post-process passes | dedust + stonfi_v2 liquidity pairing | Phase E |

Every conversion of a matcher to declarative shrinks this table; the swap
family keeps a small irreducible host core by design.

## 8. Verification methodology

The project's central discipline: **every layer gets a language-neutral gate
before the next layer starts**, and gates never break silently.

- **Conformance vectors** — `ir/expr_vectors.json` (97: every expr kind,
  builtin, operator, null-propagation, faults) and `ir/build_vectors.json`
  (25: every statement kind, rejections, lookups). Expectations are generated
  by *running the real Python evaluators* — never hand-authored.
- **Corpus A/B** — 147 real trace fixtures (`tests/traces/*.lz4`). Python twin
  scripts (scratchpad) drive the real Python engine internals and print the
  same canonical dump format as the C++ exe modes (`--match`, `--parse-dump`,
  `--build-smoke`, `--classify`); `diff` must be 0. Dump keys avoid anything
  unverified cross-language (block key = `btype#opcode@min_lt`, never
  tx-hash strings).
- **Skip tables are artifact-derived on both sides** — a matcher missing a
  host binding is skipped by the same rule from the same data, so the skip set
  itself is A/B'd and nothing drops out silently.
- **Byte-determinism of codegen** — regenerating any generated file from the
  same document must be byte-identical.
- The exact gate commands live in `mch-cpp-plan.md` §1. Run them before and
  after everything.

Python-side, the same philosophy: parity sweeps (legacy vs IR engine over the
corpus), per-spec field-level parity tests, conformance snapshots, and the
byte-identical-IR gate used for every compiler refactor.

## 9. Where things live

```
indexer/indexer/events/mch/        frontend: lexer, parser, resolver, compiler,
                                   ir_emit, expr_eval (shared sync evaluator),
                                   registry; specs/ + specs_decl/ (.mch sources);
                                   builders/ (host bindings, gitignore trap: add -f)
indexer/indexer/events/mch_ir/     Python IR engine: loader, engine, program, expr
indexer/tests/mch/                 frontend + integration tests, parity sweep,
                                   conformance snapshots
indexer/tests/mch_ir/              engine tests, vectors/ (conformance vectors)
indexer/docs/mch-*.md              specs, plans, this overview
ton-index-worker/ton-mch-engine/   C++ runtime: src/ (substrate, walker, runtimes),
                                   codegen/ (mch-codegen tool), ir/ (committed
                                   contract copies), tlb/ (message schemas),
                                   src/generated/ (regenerable, committed)
```

## 10. Current state and what's left

Done and verified (as of 2026-07-07): language feature-complete for the C++
milestone; determinism pinned; IR spec v1.0 normative; Python IR engine is the
only mch runtime in Python; ~11 matchers fully/hybrid declarative; C++ through
Phase B — full pipeline (tree → match → lookups → generated build → merge →
classify) A/B-clean on the cleared surface (4/35 matchers included; the rest
skip on missing host bindings).

Remaining: Phase C parser families (each family shrinks the skip table),
Phase D host preds/fns/shaper, Phase E flows + post-processors + serializer
boundary, Phase F Python-side destaling (incl. the `mch_ir_version`
"0.1-poc"→"1.0" bump decision), Phase G production wiring (actor-based lookup
source, priority review, deployment shape). Detailed sequencing:
`mch-cpp-plan.md` §4-§9.
