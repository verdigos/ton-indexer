# MCH Roadmap — ordered task list

**Spec:** `docs/mch-spec.md` (v0.2). **Baseline:** commit `efde513a` (Phases 1–5, 7 of the old roadmap implemented).
**Supersedes:** `docs/superpowers/plans/2026-05-19-mch-future-roadmap.md` (old 11-phase plan; its Phases 6, 8–11 are folded into the tasks below).

## How to use

Each task is one agent-session-sized. Reference a task in a prompt like:

> Do task T13 from docs/mch-roadmap.md

Rules for executing a task:
1. Read `docs/mch-spec.md` sections listed in the task's **Spec** field first.
2. Do not start a task whose **Needs** tasks are not DONE.
3. A task is DONE only when its **Accept** criteria pass and `python -m pytest tests/mch/ -v` is green (venv: `S:\Dev\H-dump\tonscan-indexator\venv0`).
4. Update the task's Status here (`todo` → `done`, add date + short note) in the same change.
5. Do not commit unless explicitly asked.

Status values: `todo`, `wip`, `done`, `blocked(<reason>)`.

---

## M1 — Core hardening

Current compiler already covers M1 language scope. These tasks close verification gaps and known holes.

### T01 — Baseline verification snapshot
Status: todo
Spec: whole doc (skim), §1.1
Needs: —
Goal: confirm the working tree (== `efde513a`) actually delivers everything the spec marks M1/M2-done: run full `tests/mch/` suite; manually verify against spec §6.5 attach-to-left cases, §6.6 XOR, §7.2 list bindings, multi-hop parents; verify anchor `where` clause is honored (wraps `test_self`); verify typed vs `Any` fields on the runtime Match dataclass. Record findings as a short feature-matrix section appended to this file (below the task list).
Accept: matrix appended; every deviation from spec has a linked new task or an existing task ID.

### T02 — Same-name captures across exclusive branches
Status: done (2026-07-03 — resolver collects occurrences with exclusive-branch paths; dup names legal across XOR / heuristic-exclusive OR branches; merged optionality; entry on multi-position capture → R007 until T10; runtime walker needed no change. 148 tests green.)
Spec: §6.2, §6.6
Needs: T01
Goal: resolver currently rejects any repeated capture name (`R005`) unconditionally. Implement the spec exception: same name legal when all occurrences sit in mutually exclusive branches (any `^` alternative; `|` alternative only when every branch is a bare concrete node with pairwise-distinct heads — same condition as the XOR-upgrade heuristic). Winning branch binds; losers default.
Accept: `(op A @x ^ op B @x)` compiles and binds correctly in a behavioral test; incompatible placements still raise R005.

### T03 — Single source of truth for Match typing + codegen integration
Status: todo
Spec: §7.1–7.3, §11.1
Needs: T01
Context (verified 2026-07-03): `codegen.py` emission logic is correct (incl. `list[T]` under recursion, btype/pred/any→`Block`, `| None`), but the module is an orphan — `emit()` is called only by its own tests, the compile facade never invokes it, `generated/` is empty, nothing imports `*_match.py`, and `docs/mch-codegen-usage.md` references a nonexistent `dev_utils/mch_codegen.py`. Runtime `_make_match_dataclass` types every field `Any`; its `block_types`/`opcodes` params are dead code. Field names/order already agree between runtime and emitted (both iterate `rm.captures`).
Goal:
1. Runtime Match and emitted dataclass generated from the same `CaptureInfo` data — unify runtime typing with `_capture_type_str` (or generate runtime class from the emitted module); remove dead params.
2. Wire `emit()` into the compile facade or a real dev script; create `dev_utils/mch_codegen.py` or fix the usage-doc reference.
3. Drop or verify the decorative header hash; remove unreachable `body or "pass"` branch.
Accept: test compiles a matcher with scalar + optional + list + btype captures and asserts runtime Match fields and emitted dataclass fields identical in name/order/type; multi-matcher file emission test; unsafe-name `ValueError` test; `emit()` reachable through a documented entry point that exists.

### T04 — Diagnostics test coverage
Status: todo
Spec: §12
Needs: —
Goal: add tests for diagnostic codes that have zero coverage: `C001_RECURSION_SHAPE_UNSUPPORTED`, `C002_UNKNOWN_BUILDER`, `C003_UNKNOWN_BLOCK_TYPE`, `W900` (stitch deferral; superseded by T05's P011), lexer STRING content path, and the three untested `R007` entry-constraint branches (undefined capture; inside recursive rule; non-op/btype head).
Accept: each listed code has at least one asserting test.

### T05 — Remove `stitch`, reserve keywords, add migration error
Status: todo
Spec: §3.5, §5.5, §12.1
Needs: T01
Goal: delete stitch grammar/AST/resolver acceptance and its parser tests. Keep `stitch partial final key ttl merge` as reserved words; parser emits `P011_STITCH_REMOVED` with hint "paired results are merged by host post-process code" when `stitch` is encountered.
Accept: stitch declaration in a spec file produces P011 with correct span; no stitch references remain in `indexer/events/mch/` outside the reserved-word list and P011.

---

## M2 — Pattern completeness

Goal of the milestone: every structural shape used by the production matchers in `blocks/jettons.py`, `blocks/dns.py`, `blocks/swaps.py`, `blocks/liquidity.py` is expressible and proven by a ported `.mch` file with field-level parity tests.

### T10 — Opcode-set anchors
Status: done (2026-07-03 — resolver validates `entry` on exclusive-alternative groups (one bare concrete-head branch per occurrence, full coverage) into `ResolvedMatcher.anchor_group`; compiler lowers the group as the anchor atom with a GenericMatcher set-membership `test_self` (opcode-set OR btype-set); maybe/recursive/non-exclusive/nested/partial-coverage/where-on-branch → R007. 168 tests green.)
Spec: §8.3 (M2 clause)
Needs: T02
Goal: allow `entry @m` where `@m` is bound on every branch of an XOR of concrete heads: `(op JettonMint @m ^ op MinterJettonMint @m)`. Anchor lowers to an opcode-set membership `test_self`.
Accept: behavioral test with a two-opcode anchor fires on both opcodes; `R007` still rejects anchors in non-exclusive alternatives.

### T11 — `produces` union
Status: done (2026-07-03 — parser accepts `IDENT ("|" IDENT)*`; AST field is now `tuple[str, ...]` with a `produces_primary` accessor; resolver validates every union member against `block_types` (C003 per unknown name); compiler adapted to `produces_primary`. 152 tests green (148 baseline + 4 new; 8 pre-existing resolver tests updated to register their dummy block type, since produces validation moved from compile-time-only to resolve-time).
Spec: §8.1
Needs: —
Goal: `produces A | B | C` — validate every name against the block-type registry; builder may return any of them.
Accept: parser + resolver tests; DNS-shaped matcher compiles.

### T12 — Mixed-direction chain conformance
Status: done (2026-07-03 — 6 structural tests pin shapes 1-5 (all already-correct per §6.4/§6.5); shape 6 `op A -> op B <- op C` redefined from silent-drop to compile-time rejection C009_MIXED_CHILD_PARENT_CHAIN_UNSUPPORTED; §6.4 sentence + §12.1 reconciled. mch suite 158 green.)
Spec: §6.4, §6.5 (CB-after-parent-edge)
Needs: T01
Goal: structural tests pinning the tricky lowerings: `op A <- op B` (anchor-headed parent edge → `parent_matcher`), `op A -> op B <- op C`, `op A <- op B -> {C; D}` (CB attaches to parent matcher). Fix lowering where tests reveal drift from spec.
Accept: each shape has a structural test asserting the exact `child_matcher`/`parent_matcher`/`children_matchers` wiring.

### T13 — Port jetton family to `.mch`
Status: todo
Spec: §14.1, §5.4
Needs: T03, T10
Goal: write `specs/jettons.mch` covering `jetton_transfer`, `jetton_burn`, `jetton_mint` (two-opcode anchor), with external builders that reuse the existing build logic from `blocks/jettons.py` (refactor bodies into shared functions callable from both the legacy matcher and the mch builder — do not duplicate logic). PTon transfer stays hand-written for now (its disambiguation is build-side; becomes declarative in M4).
Accept: integration tests run DSL and legacy matchers on real trace fixtures and assert **field-level equality** of resulting block `data` (not count parity).

### T14 — Port DNS to `.mch`
Status: todo
Spec: §8.1, §14.5 (pattern part only — build stays host-side until M4)
Needs: T11
Goal: `specs/dns.mch` with `produces change_dns | renew_dns | delete_dns` and an external builder reusing `ChangeDnsRecordMatcher` logic.
Accept: field-level parity integration test on a dns trace fixture.

### T15 — Port dedust swap to `.mch`
Status: todo
Spec: §6.5, §6.7, §7.2, §14.2-style recursion
Needs: T01, T12
Goal: `specs/dedust_swap.mch` reproducing `DedustSwapBlockMatcher` (recursive peer chain with exit matcher, children sets, OR parent). External builder reuses existing `build_block` logic via shared function.
Accept: field-level parity on at least 2 fixtures (single-hop, multi-hop). Document any pattern-inexpressible aspect as a spec issue in this file.

### T16 — Port stonfi_v2 swap end-to-end
Status: todo
Spec: §14.2
Needs: T15
Goal: upgrade the existing stonfi_v2 integration test from simplified pattern + stubbed predicate to the full spec §14.2 pattern with the real `pton_self_transfer` predicate registered; builder reuses hand-written logic.
Accept: field-level parity incl. multi-hop fixture; predicate no longer stubbed.

### T17 — Port dedust_withdraw + one of tonco/coffee
Status: todo
Spec: §6.5, §6.6
Needs: T15
Goal: breadth validation on two more protocols (`DedustWithdrawBlockMatcher`, then `ToncoSwapBlockMatcher` or `CoffeeSwapBlockMatcher` — pick whichever exposes a new shape).
Accept: field-level parity tests; new spec issues (if any) filed as tasks.

### T18 — §7.2 alignment-rule conformance suite
Status: todo
Spec: §7.2
Needs: T01
Goal: behavioral tests for list captures: unconditional step path parallel-aligned; branch-dependent captures shorter lists; `maybe $rule` absent ⇒ `[]`; trace-order (`min_lt`) element ordering; `out_transfer[-1]`-style terminal access.
Accept: suite passes; any engine fix needed is included.

---

## M3 — Integration: ordering, determinism, production wiring

Note: partial/final result merging (dedust deposit) is deliberately **not** a DSL feature — it stays host-side post-process code in both Python and C++ (spec §5.5). T24 only proves mch-produced blocks flow through the existing pass.

### T20 — `priority` directive + deterministic registration order
Status: todo
Spec: §8.6, §10.6
Needs: T01
Goal: parse/resolve `priority DEC` (default 100); compiled namespace yields matchers sorted by (priority, file name, source position). Expose ordered list for engine registration.
Accept: tests for ordering incl. tie-breaks; duplicate-priority warning emitted.

### T21 — Canonical candidate ordering in the engine
Status: todo
Spec: §10.5
Needs: —
Goal: pin candidate order (ascending `min_lt`, tie by stable `next_blocks` order) wherever the engine picks among blocks: `process_child_matcher`, `process_children_matchers`, recursion frontier. Verify against the existing YAML corpus that no production classification changes (run `tests/test_actions.py` before/after; investigate every diff).
Accept: engine change + unit tests; full corpus diff is empty or every diff is explained and approved.

### T22 — Build-rejection contract
Status: todo
Spec: §10.7
Needs: —
Goal: tests pinning: builder returns `[]` ⇒ no consumption, next matcher in order gets its attempt; builder raises ⇒ same + diagnostic logged; trace processing never aborts.
Accept: behavioral tests with two matchers sharing an anchor where the first rejects.

### T24 — Port dedust deposit liquidity (host-side merge)
Status: todo
Spec: §5.5, §14.3
Needs: T15
Goal: `specs/dedust_deposit.mch` per §14.3 — the two matchers only. Merging stays with the existing `post_process_dedust_liquidity` (unchanged); mch builders must produce the same `DedustDepositLiquidityPartial` / `DedustDepositLiquidity` classes the pass dispatches on.
Accept: field-level parity on fixtures covering: both-branch trace (post-process merges), partial-only trace (partial survives), rejection path.

### T25 — Cross-file namespace
Status: todo
Spec: §2
Needs: T01
Goal: `compile_dir` compiles all `.mch` files as one namespace: cross-file `$rule`/`opcode` references resolve; same-name opcode must agree on value; duplicate rule/matcher names error.
Accept: facade tests incl. cross-file rule ref and cross-file conflict errors.

### T26 — Production wiring behind feature flag
Status: todo
Spec: §10.6, §11.1
Needs: T13, T20, T22
Goal: `event_processing.py` builds its `matchers` list with mch-compiled matchers substituted for hand-written ones, per-matcher, controlled by a settings flag (e.g. `TON_INDEXER_MCH_MATCHERS=jetton_transfer,dns`). Default: off.
Accept: with flag on for jetton_transfer, full `tests/test_actions.py` suite passes unchanged.

### T27 — Corpus parity harness
Status: todo
Spec: §11.3
Needs: T26
Goal: harness that runs every YAML test case (`tests/test_cases/`) twice — legacy matchers vs mch-substituted — and diffs resulting actions. This is the acceptance gate for each ported matcher and, later, the cross-language fidelity backbone.
Accept: harness runnable via pytest marker or a dev_utils script; report lists per-case diff; green for all matchers ported so far.

---

## M4 — Build language

Read spec §9 fully before any M4 task. Design principle: expressions stay pure/total and IR-friendly (§11.4); every host `fn` is a counted parity liability.

### T30 — Expression language design note
Status: todo
Spec: §9.6, §11.4
Needs: —
Goal: short design doc (`docs/mch-expr-design.md`): typing rules, null semantics, builtin signatures, the ~20-op stack VM the expressions must compile to, conformance-vector format. No implementation.
Accept: doc reviewed against every expression used in spec §14.4/§14.5 plus the field mappings of stonfi_v2/dedust swap builders (paper-port them).

### T31 — Build-statement parsing + resolution
Status: todo
Spec: §4 (M4 productions), §9.1–9.5, §9.8–9.10, §12.1 (B-codes)
Needs: T30
Goal: lexer/parser/AST/resolver for `parse/let/reject/failed/broken/out/flow/omit/fn/produces switch`; diagnostics B001–B004; registries for message types, lookups, fns.
Accept: parser + resolver tests for every statement and every B-code.

### T32 — Expression evaluator + three-phase execution
Status: todo
Spec: §9.6, §9.7
Needs: T31
Goal: evaluate expressions per design note; engine executes collect → batched fetch → evaluate. Lookup batching verified (single gather per match).
Accept: behavioral tests incl. null propagation, `??`, list combinators, batched-lookup call-count assertion.

### T33 — Inline field predicates
Status: todo
Spec: §6.8 (M4 clause)
Needs: T31
Goal: `where .field == literal` / `!=` on any node; missing field ⇒ false; match-time (sync, no lookups).
Accept: behavioral test replacing a `GenericMatcher`-style data check (stonfi_v2 `has_internal_transfer` case).

### T34 — `produces switch` + declarative DNS port
Status: todo
Spec: §9.5, §14.5
Needs: T32
Goal: runtime dispatch; port DNS fully declaratively (no host builder); retire T14's external builder for the mch path.
Accept: DNS field-level parity with zero host build code.

### T35 — Declarative jetton_transfer port
Status: todo
Spec: §14.4
Needs: T32
Goal: golden case — port §14.4 verbatim; add `flow`/`reject` coverage; compare against legacy on the corpus.
Accept: corpus parity (T27 harness) for jetton_transfer with no host builder.

### T36 — Hybrid ports: stonfi_v2 + dedust swap
Status: todo
Spec: §9.10, §9.9
Needs: T35
Goal: port the two big swap builders with declarative `out`/`failed when` for the mechanical parts and declared `fn`s for the corner cases (stonfi referral split, dedust desired-route walk). Exercise `omit` on the stonfi v1 referral case if stonfi v1 gets ported here.
Accept: corpus parity; `fn` count recorded in this file (baseline for the parity-liability metric).

---

## M5 — IR & C++

### T40 — IR format specification
Status: todo
Spec: §11.4
Needs: T30 (VM design), M3 complete
Goal: `docs/mch-ir-spec.md`: versioned artifact layout (matcher table, anchors as opcode/btype sets, structural graphs, capture slots, expression bytecode, lookup plans, fn signatures, priority order), golden-fixture format.
Accept: doc + at least one hand-written golden fixture for the jetton_transfer matcher.

### T41 — Frontend emits IR; Python engine consumes it
Status: todo
Needs: T40
Goal: compiler backend serializing the resolved namespace to IR; Python engine gains an IR loader that reconstructs matchers from the artifact instead of (or verifying against) direct lowering. Same test suite must pass through the IR path.
Accept: full `tests/mch/` green with IR path enabled; golden IR fixtures diffed in CI-style test.

### T42 — Expression VM conformance vectors
Status: todo
Needs: T41
Goal: language-neutral test vectors (input stack/consts → expected result) covering every VM op and null-handling edge; Python VM passes all.
Accept: vector file + runner test.

### T43 — C++ engine: IR loader + matcher interpreter
Status: todo
Needs: T41, T42
Goal: C++ side (ton-index-worker; **build on remote Linux server only — never locally**) loads the IR artifact, reconstructs matcher programs over its Block representation, implements the VM, batched lookups via actor requests.
Accept: C++ unit tests green on the golden IR fixtures + VM vectors.

### T44 — Cross-language fidelity harness
Status: todo
Spec: §11.3
Needs: T43, T27
Goal: run the shared trace corpus through both engines; assert identical fired-matcher sets, consumed-block sets, and capture bindings.
Accept: harness green on the full corpus for every ported matcher.

### T45 — Hot reload of IR artifacts
Status: todo
Needs: T43
Goal: C++ worker reloads the IR artifact on change/signal without binary rebuild.
Accept: demonstrated reload with a matcher edit on the dev deployment.

---

## Feature matrix (maintained by T01)

_(appended by T01 — current implementation status vs spec v0.2)_
