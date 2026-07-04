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
Status: in-progress — both silent-drop cases resolved (2026-07-04). Bug 1 (flat `… <- op X -> {CB} <- Y`) is now LOWERED correctly: `_lower_parent_chain_from` walks the tail as a single interleaved chain, so a children block no longer terminates parent-chain collection; X gets BOTH children_matchers and parent_matcher, identical to the grouped form. dedust_deposit.mch un-grouped (both matchers), 8 fixtures green. Bug 2 (upstream atoms before the anchor, `$rule -> anchor…` / B.2 CHILD-incoming) now ERRORS with `C010_UPSTREAM_ATOMS_UNSUPPORTED` (was silent drop). Structural + diagnostic tests in tests/mch/test_compiler_structural.py; the B.2 behavioral test in test_compiler_behavioral.py flipped from expecting silent success to asserting C010. Remaining T04 goal (zero-coverage tests for the other codes below) still todo.
Spec: §12
Needs: —
Goal: add tests for diagnostic codes that have zero coverage: `C001_RECURSION_SHAPE_UNSUPPORTED`, `C002_UNKNOWN_BUILDER`, `C003_UNKNOWN_BLOCK_TYPE`, `W900` (stitch deferral; superseded by T05's P011), lexer STRING content path, and the three untested `R007` entry-constraint branches (undefined capture; inside recursive rule; non-op/btype head). ~~Also ADD a new diagnostic (T16 finding): atoms upstream of the anchor in a sequence head (`$rule -> anchor…` shape) are currently SILENTLY DROPPED by `_lower_parent_chain_from`~~ — DONE: emits `C010_UPSTREAM_ATOMS_UNSUPPORTED`.
~~SECOND silent-drop case (port wave 2026-07-04, dedust_deposit): in a flat parent chain `… <- op X -> { CB } <- Y`, the trailing `<- Y` parent edge after a children block is silently dropped~~ — DONE: the flat form now lowers identically to the grouped form; the dedust_deposit.mch workaround grouping is removed.
Accept: each listed code has at least one asserting test; both silent-drop shapes error or lower correctly (both silent-drop shapes: DONE).

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
Status: done (2026-07-03 — specs/jettons.mch (transfer/burn/mint incl. two-opcode anchor); build cores extracted in blocks/jettons.py (`build_jetton_{transfer,burn,mint}_core`), shared by legacy matchers and mch builders in mch/builders/jettons.py; 6 field-level parity tests on real fixtures (both mint anchor opcodes + failed path) in tests/mch/integration/test_jettons_family.py; mch suite 174 green, test_actions corpus 123 green. T03 dependency waived by user — builders consume the untyped runtime Match.)
Spec: §14.1, §5.4
Needs: T03, T10
Goal: write `specs/jettons.mch` covering `jetton_transfer`, `jetton_burn`, `jetton_mint` (two-opcode anchor), with external builders that reuse the existing build logic from `blocks/jettons.py` (refactor bodies into shared functions callable from both the legacy matcher and the mch builder — do not duplicate logic). PTon transfer stays hand-written for now (its disambiguation is build-side; becomes declarative in M4).
Accept: integration tests run DSL and legacy matchers on real trace fixtures and assert **field-level equality** of resulting block `data` (not count parity).

### T14 — Port DNS to `.mch`
Status: done (2026-07-03 — specs/dns.mch (`produces change_dns | renew_dns | delete_dns` union, anchor `op ChangeDnsRecord @msg -> maybe op 0xffffffff`); shared build core `build_change_dns_core` extracted in blocks/dns.py (dns.py:36), 3-way content dispatch, called by both legacy matcher and mch builder in mch/builders/dns.py; excess/bounce left at defaults (true) + explicit optional bounce child — mirrors legacy consumed set exactly. NOTE: corpus has ZERO DNS fixtures (all 147 traces scanned, opcode 0x4eb1f0f9 absent; no YAML case), so parity is proven on synthetic-but-faithful trees built via the real Transaction/Message -> to_tree -> init_block path (5 field-level parity tests: change/renew/delete + bounce-consumed + failed-flag). mch suite 179 green (174 + 5), test_actions 123 green.)
Spec: §8.1, §14.5 (pattern part only — build stays host-side until M4)
Needs: T11
Goal: `specs/dns.mch` with `produces change_dns | renew_dns | delete_dns` and an external builder reusing `ChangeDnsRecordMatcher` logic.
Accept: field-level parity integration test on a dns trace fixture.

### T15 — Port dedust swap to `.mch`
Status: done (2026-07-03 — specs/dedust_swap.mch; lowered tree mirrors legacy exactly (anchor children_matchers=[Or([direct payout chain, recursion]), maybe notification], parent Or of jetton_transfer/DedustSwap, recursion step = Or of peer/external each with optional-notification children set, exit = payout chain). Shared core `build_dedust_swap_core` extracted in blocks/swaps.py (helpers moved to module level as `_parse_dedust_steps`/`_get_dedust_jetton_swap_sequence`); legacy `build_block` is now a thin wrapper — test_actions corpus 123 green. mch builder in mch/builders/dedust.py. 4 parity tests in tests/mch/integration/test_dedust_swap.py: 2 multi-hop (recursion), 1 single-hop TON-in (`op DedustSwap` parent arm), 1 both-reject-on-build-error (§10.7); corpus has no jetton-in single-hop and no failed-swap trace. mch suite 186 green (179 + 7). Two additive compiler changes, see notes below.)
Spec: §6.5, §6.7, §7.2, §14.2-style recursion
Needs: T01, T12
Goal: `specs/dedust_swap.mch` reproducing `DedustSwapBlockMatcher` (recursive peer chain with exit matcher, children sets, OR parent). External builder reuses existing `build_block` logic via shared function.
Accept: field-level parity on at least 2 fixtures (single-hop, multi-hop). Document any pattern-inexpressible aspect as a spec issue in this file.

T15 notes (spec issues found and how they were resolved):
- **Anchor with both a children set and a parent edge** (`op A -> {…} <- P`) was mis-rejected with C009, making the dedust anchor shape inexpressible; per §6.5 a children block does not advance the chain cursor, so the parent edge still belongs to the anchor. Fixed in the compiler (`_lower_child_chain_from` exempts CB-only prefixes from C009; `_lower_parent_chain_from` skips leading CB atoms) — additive: a previously compile-erroring shape became supported, no accepted pattern changes lowering. Structural test shape 7 + behavioral test added; spec §6.4/§6.5/§12.1 updated.
- **Builders need the full consumed set** (host-contract, §10.7/§11): the legacy dedust build scans `other_blocks` freely (`find_messages`, `find_call_contracts`, `in include` membership); captures cannot reconstruct it because blocks matched by a recursion's **exit branch** have no bindings (the `_MchRecursiveMatcher` walker records only repeating-step iterations), and captures positioned after the recursion in traversal order (the anchor's own `maybe notification` item, the parent) are unbound in the multi-hop case (engine `set()` dedup destroys positional order). Resolved additively: the synthesized `build_block` sets `Match._mch_consumed = [anchor] + other_blocks`; documented in spec §11.1; behavioral test added. DSL captures in dedust_swap.mch are kept for pattern fidelity but the builder works exclusively off the consumed set.
- **§7.2 alignment gaps for T18**: (a) exit-branch captures never bind (spec reading suggests they should bind once); (b) list captures under `maybe` inside the step path append `None` for absent per-iteration elements (parallel-aligned with gaps) rather than producing a shorter list as §7.2's branch-dependent wording implies; (c) captures after a recursion in the same children set / parent position silently stay `None` (only same-sequence positions are guarded by C007). Worth folding into the T18 conformance suite.

### T16 — Port stonfi_v2 swap end-to-end
Status: done (2026-07-03 — specs/stonfi_v2_swap.mch rewritten from the simplified/stubbed version to a full legacy-equivalent pattern (anchor hop in matcher pattern + `$swap_hop` recursion for hops 2..n, XOR in/out alternatives, `pred pton_self_transfer` on a bare pred node). Real predicates registered: `pton_self_transfer` (exact legacy `_validate_intermediate_pton_transfer` logic, extracted to module-level `validate_stonfi_v2_intermediate_pton_transfer` in blocks/swaps.py, shared by both paths) and `no_internal_transfer` (M4 inline-predicate candidate). Shared core `build_stonfi_v2_swap_core(block, peer_swap_pairs, in_transfer, out_transfer)` extracted in blocks/swaps.py; legacy `build_block` is now a LabelBlock-scan + merge wrapper. mch builder in mch/builders/stonfi.py derives the label roles structurally from `Match._mch_consumed` (captures can't bind: exit-less recursion lists + post-recursion parent, same as T15). 6 parity tests at T13 depth in tests/mch/integration/test_stonfi_v2_swap.py (replaced the old count-parity test): 2 single-hop, multirouter 2-hop, jetton→TON→jetton intermediate-pton 2-hop (real predicate accept path), TON-in arm in a multi-dex trace, two-swaps-in-one-trace. mch suite 198 green (193 − 1 replaced + 6), test_actions 123 green. No compiler/resolver changes. §14.2 deviations + findings in notes below.)
Spec: §14.2
Needs: T15
Goal: upgrade the existing stonfi_v2 integration test from simplified pattern + stubbed predicate to the full spec §14.2 pattern with the real `pton_self_transfer` predicate registered; builder reuses hand-written logic.
Accept: field-level parity incl. multi-hop fixture; predicate no longer stubbed.

T16 notes (§14.2 deviations — each a spec fix or M4 inline-predicate case — and findings):
- **Anchor cannot live inside the recursive rule** (§14.2 puts `@swap` in `$swap_step`; entry on a list binding is R007). Port re-anchors: first hop's swap/payout/out-alternative in the matcher pattern, hops 2..n via `rule swap_hop`. Also, §14.2's `$in_transfer -> $swap_step` head shape does not lower: atoms upstream of the anchor are **silently dropped** by `_lower_parent_chain_from` (only anchor-headed parent edges and the B.1/B.2 tail variants lower). Spec issue: fix the §14.2 example or add upstream-head parent lowering; at minimum the silent drop deserves a diagnostic.
- **Anchor `-> chain <- parent` is C009**; the payout chain is wrapped in a one-item children set (`op Swap -> { payout-chain } <- in-alt`, the T15 §6.5 CB exemption). Semantics delta: `children_matchers` is first-match-wins per matcher, legacy `child_matcher` consumes *every* matching child — identical while a swap has exactly one pay_to child (always, in corpus and per protocol).
- **§14.2's out-arm `op JettonInternalTransfer @out where pton_self_transfer` is idealized.** The real legacy arm is `btype jetton_transfer where no_internal_transfer -> pred pton_self_transfer -> op JettonNotify`: the data check becomes host predicate `no_internal_transfer` (M4 inline-predicate case: `.has_internal_transfer == false`), and the payload validator sits on a bare `pred` node because the legacy GenericMatcher has no opcode constraint.
- **`^` where legacy has ordered OrMatcher.** Same-name captures (`@in_transfer`, `@out_transfer`, hop captures) require declared exclusivity (R005), but stonfi's out/hop branches genuinely overlap by prefix (bare-jt branch is a prefix shape of the pton-chain branch). Safe today: the recursion destructures the rule's top alternative into an ordered OrMatcher, and a matcher-level ExclusiveOr's sum==1 `test_self` is never consulted on the try_build path — but it is a declared-exclusivity lie. M4 candidate: ordered `|` alternatives with same-name captures (first-match-wins).
- **Legacy's optional pton sub-chain unrolled into branches.** Legacy branch 1 is `jt_no_internal -> maybe(pred -> notify -> maybe swap)`; the port needs the chain mandatory in branch 1 with the bare-jt branch as fallback — consumption-equivalent because everything branch 1 rejects lands in branch 2 with the same head block.
- **Terminal pton transfer consumed pattern-side.** Legacy leaves the terminal 0x01f3835d call unconsumed at match time and merges it at build via `additional_blocks_to_include`; the mch pattern consumes it via `{ maybe op PTonTransfer }` on the bare-jt branches and the builder ignores the core's returned additional list. Final absorbed set identical.
- **Engine duplicates a recursion's entry block** in the consumed list (base `build_block` returns `[block] + blocks` and `blocks` already contains the entry): consumed-set-driven builders must dedup (see `_derive_parts` in mch/builders/stonfi.py). Fold into the §11.1 `_mch_consumed` contract or fix engine-side alongside T19/T21. *(Resolved in T18: synthesized build_block de-duplicates `_mch_consumed` order-preservingly; §11.1 updated; stonfi builder dedup removed.)*
- **Predicate totality:** the registered `pton_self_transfer` wraps the exact legacy logic in try/except — the capture walker probes predicates against arbitrary consumed blocks during extraction, while engine-time inputs are always children of a matched jetton_transfer (where the legacy function is total). Decision behavior is unchanged.
- **Corpus gaps** (no recording per task rules): no failed stonfi_v2 swap (pay_to exit code ≠ ok — `destination_asset` fallback via `_get_stonfi_v2_target_asset_from_notification` and pool-account recursion unexercised), no terminal TON/pton out-transfer, no broken-flag trace (jetton wallet lookup miss).

### T17 — Port dedust_withdraw + one of tonco/coffee
Status: done (2026-07-03 — specs/dedust_withdraw.mch + specs/coffee_swap.mch; coffee chosen (tonco inexpressible, filed as T28). Shared cores extracted: `build_dedust_withdraw_core` (liquidity.py) + `build_coffee_swap_core` (swaps.py), legacy matchers now thin wrappers. Withdraw builder is fully capture-driven (first port with no `_mch_consumed`); coffee builder derives labeled-equivalents structurally from the consumed set. 7 parity tests (3 withdraw synthetic + 4 coffee recorded). mch suite 193 green (186 + 7), test_actions 123 green.)
Spec: §6.5, §6.6
Needs: T15
Goal: breadth validation on two more protocols (`DedustWithdrawBlockMatcher`, then `ToncoSwapBlockMatcher` or `CoffeeSwapBlockMatcher` — pick whichever exposes a new shape).
Accept: field-level parity tests; new spec issues (if any) filed as tasks.

T17 notes:
- **Identical-heads children set is deterministic** (withdraw): two children items sharing head `op DedustPayoutFromPool` consume first-match-wins in candidate order (§6.5 algorithm), and the positional capture walker assigns `@payout_1`/`@payout_2` in exactly the legacy `labeled('payout_1'/'payout_2')` order — pinned by parity fixtures with both payout orders (jetton-first and TON-first). No spec issue; no `_mch_consumed` needed since the pattern has no recursion.
- **Tonco is inexpressible with the current recursion strategy** — filed as T28. Coffee justification: recursion step is a two-node chain (swap → success event per unrolling), same-name `@in_transfer` capture across XOR parent branches (first port to exercise T02), and a 3-way mixed op/btype exit alternative — all new vs jettons/dns/dedust_swap.
- **Corpus gap**: no trace contains both JettonBurn and DedustPayoutFromPool (checked all 147 raw opcode sets + legacy matcher run: 0 hits), and no YAML dedust withdraw case exists — withdraw parity uses T14-style synthetic-but-faithful trees (real Transaction/Message → to_tree → init_block, prerequisite jetton matchers replayed). Coffee gaps: no failed-swap fixture (swap_internal without success event) and no CoffeePayout-vs-CoffeeNotification terminal distinction in the corpus; covered fixtures: 2-hop recursion, single-hop TON-out, native-TON-in (`op CoffeeSwapNative` arm), coffee leg inside a multi-dex trace.
- **Walk-compare extended** (dev_utils/mch_walk_compare.py, +5 scenarios): withdraw/coffee show zero consumption differences (L_only=M_only=0, same blocks, same order) but pairwise `replace` steps at every legacy `labeled()` site — `LabeledBlockConstructorMatcher` flattens its wrapped alternative into a single lambda `test_self` and emits transient LabelBlocks, while mch probes per-branch. Walk-level identity is structurally unattainable for label-using legacy matchers; relevant to T19's conventions (ratio is meaningless there — use L_only/M_only).
- No compiler/resolver changes were needed.

### T28 — Tonco swap port (needs cyclic-descent recursion strategy)
Status: todo
Spec: §10.3, §16
Needs: T17
Context (T17 finding): `ToncoSwapBlockMatcher` recursion is a **cyclic child-matcher graph** (`swap.child = payout; payout.child = Or([...intermediate chains ending in the same swap matcher..., optional finals])`) — plain recursive descent where the Or tries hop-continuation branches BEFORE the optional final-transfer branches. `_MchRecursiveMatcher` (the only strategy, §10.3) walks a frontier trying the exit FIRST per block; tonco's step and exit branches both start `swap -> payout`, so the exit would fire prematurely on every multi-hop chain (hop 2 of a 3-hop swap matches the exit since its distinguishing children are behind `maybe`). Faithful porting needs either (a) a second recursion strategy lowering self-refs at chain ends to a cyclic matcher graph (RecursionStrategy protocol already exists in recursion.py for this), or (b) negative lookahead in the language (§16 territory). Also needed: `maybe`-branch semantics inside alternatives (legacy uses optional=True branches inside OrMatcher, which OrMatcher.try_build ignores — quirk to pin before porting).
Goal: pick (a) or (b), implement minimally, port tonco with field-level parity incl. the failed-swap target-extraction path (corpus has a failed tonco fixture: `Tonco swap TON -> USDT failed`).
Accept: parity tests incl. multi-hop + failed swap; walk-compare scenario added.

### T18 — §7.2 alignment-rule conformance suite
Status: done (2026-07-03 — tests/mch/test_recursion_captures.py, 11 conformance tests (3-hop/1-hop/zero-hop shapes, trace-order, `[-1]` terminal access, `maybe $rule` absent ⇒ `[]`, gap-aligned Nones for maybe-in-step AND losing step/exit alternatives, exit binds once, post-recursion binding multi/single/zero-hop, cross-match stale-state isolation, `_mch_consumed` dedup). All three T15 gaps settled — two engine fixes + one spec blessing, see decision log below. §7.2 rewritten to the step/exit-branch alignment rule; §0/§10.3/§11.1/§12.1/§14.2 updated to match; port spec comments (dedust_swap.mch, stonfi_v2_swap.mch, builders/coffee.py, builders/stonfi.py) de-staled. T16 entry-dup stretch done: synthesized build_block de-duplicates `_mch_consumed` (order-preserving, first wins), stonfi `_derive_parts` local dedup removed. No new W-codes (both remaining gaps got engine fixes, nothing left to warn about). mch suite 209 green (198 + 11), test_actions 123 green.)
Spec: §7.2
Needs: T01
Goal: behavioral tests for list captures: unconditional step path parallel-aligned; branch-dependent captures shorter lists; `maybe $rule` absent ⇒ `[]`; trace-order (`min_lt`) element ordering; `out_transfer[-1]`-style terminal access. Include the three T15-found gaps (see T15 notes: exit-branch captures never bind; gap-aligned `None`s vs "shorter list" wording; post-recursion captures silently `None`) — resolve each by fixing engine OR amending §7.2, per case.
Accept: suite passes; any engine fix needed is included.

T18 decision log (per T15 gap):
- **(a) Exit-branch captures never bind → ENGINE FIX.** `_MchRecursiveMatcher` now records the pure exit's consumed blocks (`_mch_exit_blocks`); the capture walker walks the exit matcher against them once and appends one element per exit-owned capture (bound block or `None` for a losing sub-branch inside the exit). No per-unrolling padding for exit captures: frontier-exhaustion termination and never-ran leave them `[]`. Step-vs-exit ownership comes from `_collect_capture_names` over the lowered step/exit subtrees (attached at `_lower_recursive_rule`); this also stopped exit-only captures from accumulating `[None]*n_steps` and stopped cross-padding between two different recursive rules in one matcher.
- **(b) `maybe`-in-step appends `None` per absent element → SPEC AMENDED (gap-alignment blessed).** §7.2 now states: step-branch captures produce exactly one element per unrolling, parallel-aligned, with explicit `None` gaps for losing alternative branches / absent `maybe` subtrees — deterministic and preserves per-hop correlation. "Branch-dependent lists may be shorter" wording removed; §14.2's Match comment updated accordingly.
- **(c) Post-recursion captures silently `None` in multi-hop → ENGINE FIX.** Root cause was the walker giving up after a recursion (engine `set()` dedup shuffles the recursion's slice internally, so positional walking through it is impossible) — but the slice's LENGTH is exact: `_MchRecursiveMatcher.try_build` records `_mch_result_len` and the walker advances the cursor by exactly that, so later children-set items and the parent chain bind on multi-hop matches too (dedust @notification/@in_transfer positions). Participation is verified by entry-block identity against the current consumed list (`_mch_entry_block`, recorded only on success) — this also fixed a latent stale-state leak where a matcher reused across traces replayed the previous trace's iteration blocks when the recursion was absent. Consumed RESULTS unchanged (matching untouched — extraction only), so no T21 ordering interference. C007 stays for same-sequence chaining after `$rule` (the engine structurally ignores those atoms); §10.3/§12.1 now say precisely that.

### T19 — Walk-parity follow-ups (from walk-compare, 2026-07-03)
Status: todo
Spec: §6.6 (engine contract note), §10.5
Needs: T15
Context: `dev_utils/mch_walk_compare.py` proved jettons/dns walks identical to legacy (11 fixtures, ratio 1.000) and dedust divergent only by behaviorally-inert OR→XOR class swaps. Two follow-ups:
1. Conformance test placing an exclusive alternative where `test_self` IS consulted directly (recursion step head; nested inside another alternative) — the only placements where OR vs XOR becomes observable. Pin behavior structurally + behaviorally.
2. Perf: generated `build_block` re-walks the lowered tree to bind captures after matching (`_extract_captures`→`_walk_matcher`, re-calling `test_self` per consumed block — measured 6–25 redundant probes per successful match). Bind captures during the matching walk instead; re-run mch_walk_compare after to confirm walk parity holds and extraction-phase probes are gone.
Accept: both tests green; zero extraction-phase test_self events in walk-compare output; both suites green.

---

## M2R — Remake wave (inexpressible legacy matchers)

Origin: the 2026-07-04 port wave + follow-up taxonomy classified every matcher the port wave could not express. Verdict (user, 2026-07-04): most are *workarounds around legacy-engine limits*, not domain requirements — remake them in the mch paradigm instead of porting the hacks.

**Remake gate (binding, user decision 2026-07-04):** the internal pattern/walk may differ freely from legacy, but OUTPUT must be identical — produced block btype, `data` (field-level), `failed`/`broken`, `value_flow`, consumed event-node set, **and block topology** (`children_blocks`/`next_blocks` exactly as `serialize_blocks` sees them) — the action hierarchy derives from topology, so block parity is not optional.

Acceptance convention for every R-task below (in addition to per-task Accept):
1. Three-way parity: legacy == compiled path == v2 IR engine, on every fixture.
2. `tests/test_actions.py` (123) green, `tests/mch` + `tests/mch_ir` green.
3. Sweep row added to `tests/mch/integration/test_parity_sweep.py`.
4. No-fixture protocols use T14-style synthetic-but-faithful trees (real `Transaction/Message → to_tree → init_block`); recording new traces is out of scope (user-side, live DB).
5. T13 convention where legacy logic is reused (`build_*_core` extraction, legacy matcher becomes thin wrapper); a remake MAY instead write a fresh builder when the legacy build is part of the hack — but then field-parity fixtures carry the whole burden, so cover every legacy branch.

Suggested execution order: R01→R05 (pure remakes, no new features) → R06 `shape` (unlocks R07/R08) → R09/R10 (anchor features) → R11 (=T28) → R12 (after M4 T33).

### R01 — Remake: telegram NFT purchase
Status: done (2026-07-04 — specs/telegram_nft.mch: legacy imperative prev-walk → declarative `op NftOwnershipAssigned <- btype ton_transfer -> {AuctionFillUp; maybe AuctionFillUp}` (T12 shape); `build_telegram_nft_purchase_core` shared; 3-way parity on Fragment fixture incl. topology; corpus mini-sweep 0 diffs. SCOPE FLAG for T26: mch pattern covers ton_transfer-funded purchases only — telemint-mint parent variant + non-purchase orphan NftOwnershipAssigned stay legacy-only until extra XOR parent alternatives + a fallback matcher are added.)
Tier: opus
Needs: —
Legacy: `TelegramNftPurchaseBlockMatcher` (`blocks/nft.py:299`).
Fixtures: `nft.yaml` "Fragment purchase" ✓.
Goal: re-derive the trace shape from the legacy matcher + fixture, express directly in `.mch` (expected: plain anchor + children/parent edges; the legacy contortion is a workaround, drop it). Builder reuses legacy field logic via core extraction.
Accept: field + topology parity on the Fragment fixture; remake gate above.

### R02 — Remake: jvault family
Status: done (2026-07-04 — specs/jvault.mch all 4 matchers + shared `$jvault_referrals`; legacy NESTED referral recursion flattened to a single recursion level (finding confirmed: real shape is flat); 4 `build_jvault_*_core` extracted, legacy = thin wrappers; 3-way parity on all 11 jvault.yaml fixtures incl. consumed-set/topology/value_flow; sweep row added; no engine changes.)
Tier: opus
Needs: —
Legacy: `JVaultStakeBlockMatcher` / `JVaultUnstakeBlockMatcher` / `JVaultClaimBlockMatcher` / `JVaultUnstakeRequestBlockMatcher` (`blocks/jvault.py:104/207/317/396`).
Fixtures: `jvault.yaml` ✓.
Goal: legacy nests recursion where the real shape is flat — port-wave finding: nested recursion flattens to plain children sets + at most one recursion level. One `specs/jvault.mch` for all four matchers.
Accept: parity on every jvault YAML case; remake gate.

### R03 — Remake: tgBTC mint
Status: done (2026-07-04 — specs/tgbtc.mch: re-anchored at head 0x3F781D24 (legacy anchored tail + scanned up); variable-length trunk via `any`-headed recursion `rule mint_chain = (any @mid -> maybe $mint_chain ^ op Tail -> {SuccessLog; btype jetton_mint})` — reusable linear-chain idiom; `build_tgbtc_mint_core` shared; 3-way parity on all 4 tgbtc.yaml mint fixtures. FINDING → spec §11.1 note: ContractDeploy side-effect blocks live in `children_blocks` (patterns can't consume them) — builders must sweep them from the consumed set to hold topology parity. Documented deviation: legacy also collects non-path ancestor siblings; all corpus mints are linear so consumed sets identical — a mid-chain fork would diverge (no such trace).)
Tier: opus
Needs: —
Legacy: `TgBTCMintBlockMatcher` (`blocks/tgbtc.py:54`).
Fixtures: `tgbtc.yaml` — 4 mint cases ✓ (incl. two "very long" chains). NOTE: `tgbtc.yaml` currently carries unrelated working-tree edits — do not include them in any MCH commit.
Goal: re-anchor at the distinctive head `0x3F781D24` with child recursion for the long chain (port-wave sketch); legacy anchors elsewhere and scans. Validate sketch against legacy source before writing the spec.
Accept: parity on all 4 mint cases incl. very-long chains; remake gate.

### R04 — Remake: stonfi_v2 withdraw liquidity
Status: done (2026-07-04 — specs/stonfi_v2_withdraw.mch: sibling scan killed via `{payout_1; payout_2}` children set (dedust_withdraw family). Legacy's no-shared-head OrMatcher parent (normal vs refund) SPLIT into two mutually-exclusive matchers: `stonfi_v2_withdraw` (btype jetton_burn anchor) + `stonfi_v2_withdraw_refund` (op 0x0f98e2b8 anchor, `<-` refund-request parent) — R09 multi-kind anchors would re-unify them. `build_stonfi_v2_withdraw_core` shared; synthetic 3-way parity: jetton-first, ton-first, refund, reject. Edge flag: remake requires exactly 2 payouts (protocol-faithful); legacy tolerated 1.)
Tier: opus
Needs: —
Legacy: `StonfiV2WithdrawLiquidityMatcher` (`blocks/liquidity.py:834`).
Fixtures: NONE in corpus (no stonfi_v2 liquidity YAML) → synthetic trees per convention #4.
Goal: legacy does an imperative sibling scan; remake sketch: `anchor <- ( router -> {payout_1; payout_2} )` — burn anchored, both payouts as a children set on the parent router (same shape family as dedust_withdraw, T17). Validate sketch against legacy.
Accept: synthetic-tree parity covering both payout kinds (jetton / TON) and both orders; remake gate.

### R05 — Remake: nominator pool withdraw
Status: done (2026-07-04 — specs/nominator.mch: legacy anchors the ProcessWithdrawRequests call and fans out N blocks from one match leaving the call unconsumed — unsupported by mch build path (1 builder call → 1 block; sharing the call across produced blocks would corrupt parent/compact_connections). Re-anchored per payout: `btype ton_transfer @payout where nominator_pool_withdraw_parent` — predicate inspects `.previous_block` WITHOUT capturing it, so the call stays unconsumed exactly like legacy; engine's per-block loop reproduces N-payout granularity for any N. `build_nominator_pool_withdraw_core` shared; synthetic 3-way parity ×4 (single, two payouts = granularity pin, interface reject, aborted-tx). Legacy quirk reproduced not fixed: `.failed` never set on this block type.)
Tier: sonnet
Needs: —
Legacy: `NominatorPoolWithdrawMatcher` (`blocks/staking.py:407`).
Fixtures: NONE (no nominator YAML) → synthetic trees; if the shape can't be faithfully synthesized (text-comment-triggered flows), set `blocked(fixtures)` and stop.
Goal: legacy re-anchors awkwardly; remake with a direct anchor per the port-wave note ("re-anchor per payout"). Verify output granularity stays identical to legacy (same number of produced blocks per trace).
Accept: synthetic parity or explicit blocked status with findings; remake gate.

### R06 — `shape` directive (post-build tree surgery in host fn)
Status: todo
Tier: fable
Needs: —
Spec: add §8.7 (directive), §11.1 (registry + contract), IR doc key.
Context: exactly two legacy matchers mutate the tree beyond `merge_blocks`: `nft.py:244` and `liquidity.py:817` — both `EmptyBlock()` + `insert_between` proxy insertion. `merge_blocks` is already licensed build-phase tree mutation; this is the same class, so it stays host-side but declared.
Goal:
1. Matcher directive `shape <fn>`; 6th registry `shapers`; fn signature `(produced_block, match) -> None`, invoked by the engine AFTER `merge_blocks`, both paths (compiled + `mch_ir`).
2. Contract (spec text): shaper may mutate only the produced block's region — its `next_blocks`/`children_blocks`/consumed set; never upstream siblings outside the region.
3. IR matcher-record key `"shape": "<fn>"`; loader hard-fails on missing shaper (mch_ir convention); emitter round-trip test.
4. Diagnostics: unknown shaper at compile/resolve = error (C-code); directive without registry entry = same.
Accept: synthetic test — shaper inserts EmptyBlock via `insert_between`, three-way topology parity asserted; spec + `docs/mch-ir-poc.md` updated; both suites green.

### R07 — Remake: getgems NFT purchase
Status: todo
Tier: opus
Needs: R06
Legacy: `GetgemsNftPurchaseBlockMatcher` (`blocks/nft.py:193`; surgery at nft.py:244).
Fixtures: `nft.yaml` — "Getgems purchase", "Getgems auction purchase" ×2 ✓.
Goal: pattern remade declaratively; the EmptyBlock proxy insertion moves into a registered `shape` fn (verbatim legacy surgery). Builder via core extraction.
Accept: parity on all 3 getgems cases incl. inserted-proxy topology; remake gate.

### R08 — Remake: stonfi_v2 provide liquidity (partial/final realignment)
Status: todo
Tier: fable
Needs: R06
Legacy: `StonfiV2ProvideLiquidityMatcher` (`blocks/liquidity.py:732`; surgery at liquidity.py:817; cross-branch `deposit_part` pairing via GenericMatcher over prior mch-produced blocks).
Fixtures: NONE → synthetic trees.
Goal: architecture realignment (class C): split into partial/final matchers dedust-style; pairing/merging stays a host post-process pass (mirror `post_process_dedust_liquidity` — merging is host-side per spec §5.5, hard domain rule); EmptyBlock surgery via `shape`. This intentionally does NOT reproduce the legacy matcher's internal structure — only its outputs.
Accept: synthetic parity: both-branch trace (post-process merges), single-branch trace (partial survives); topology incl. proxy block identical; remake gate.

### R09 — Multi-kind anchors
Status: todo
Tier: opus
Needs: —
Spec: §8.3 (extend the T10 M2 clause).
Goal: `entry @m` on an XOR whose branches mix `op` and `btype` heads, each branch optionally carrying `where`. Anchor lowers to union set-membership (opcode-set ∪ btype-set) + per-branch `where` after membership. Relax the corresponding R007 arms. Survey `event_processing.py` matchers whose `test_self` is `opcode == X or btype == Y`-shaped and list them in this task's done-note (port-wave estimate: ~7 unblocked).
Accept: behavioral tests (fires on both kinds; where-gated branch); R007 still rejects non-exclusive/maybe placements; survey list recorded.

### R10 — `pred` anchors
Status: todo
Tier: opus
Needs: —
Spec: §8.3.
Legacy driver: `NftMintBlockMatcher` (`blocks/nft.py:367`) anchors on a predicate (deployment-count style test), no concrete head.
Goal: allow `entry` on a `pred`-headed node. No opcode prefilter possible → engine falls back to full-scan test_self; measure cost on the corpus and record it. If cost is unacceptable, alternative is explicit: leave pred-anchored matchers hand-written and close this task with that finding — do not force it.
Accept: either working pred-anchor with measured overhead + behavioral test, or a documented rejection with numbers.

### R11 — Tonco swap (= T28, step-first recursion strategy)
Status: todo
Tier: fable
Needs: —
Pointer task: execute **T28** (see M2 above — cyclic child-graph recursion; strategy (a) cyclic descent vs (b) negative lookahead decision lives there). Listed here because it is part of the remake wave's feature batch. Update T28's own status when done.

### R12 — Remake: stonfi v1 swap
Status: todo
Tier: fable
Needs: T33 (M4 inline field predicates); revisit `omit` (§9.9/T36) if predicates alone don't kill the surgery
Legacy: `StonfiSwapBlockMatcher` (`blocks/swaps.py:189`) — build-side consumption surgery: for `ok_ref` exit codes the referral subtree is bfs-removed from `other_blocks` (build edits the consumed set).
Fixtures: `stonfi-swaps.yaml` ✓ rich — referral payout, failed, receiver≠sender.
Goal: with inline field predicates the referral/out disambiguation moves to match time and the consumption surgery disappears; whatever remains imperative goes into a declared `fn`. Last remake — deliberately after M4 lands T33.
Accept: parity on every stonfi-swaps YAML case incl. referral split + failed; remake gate.

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
Primary known hazard (walk-compare, 2026-07-03): `set()` usage in `RecursiveMatcher.process_child_matcher` (basic_matchers.py:233) and in mch's recursive subclass reorders consumed blocks across runs and propagates to sibling visitation — affects legacy and mch equally; target it first.
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

---

## Port wave 2026-07-04 (branch mch-engine-v2) — broader e2e of the v2 IR engine

Eight matchers ported in one wave (3 complex / 2 medium / 3 easy), each with the T13 convention (`build_*_core` extraction, `.mch` spec, mch builder, field-level parity) PLUS a three-way gate: legacy == compiled == IR-engine on every fixture. Sweep harness (`test_parity_sweep.py`) extended to all new specs: 19 specs, 462 sweep pairs green ×3 runs. Suites: tests/mch + tests/mch_ir = 927 passed / 1 skipped; tests/test_actions.py = 123.

Ported: `dedust_deposit` final+partial (specs/dedust_deposit.mch; post-process merge parity proven — isinstance dispatch works on mch-produced blocks), `evaa_withdraw` (specs/evaa.mch, 4 host preds for EVAA header-skip + service comments), `layerzero_receive` (specs/layerzero.mch; legacy dup label → distinct captures `@execute`/`@oapp_callback`, outer block is the bound one), `tonstakers_withdraw` (specs/tonstakers.mch; first produces-union e2e), `tonco_deposit_liquidity` (specs/tonco_liquidity.mch), `ethena_deposit` (specs/ethena.mch; first btype-anchored spec through the full IR chain), `vesting_send_message` (specs/vesting.mch), `coffee_create_vault` (specs/coffee_liquidity.mch).

Findings folded into tasks/spec:
- NEW T04 silent-drop case: trailing parent edge after a children block in a flat chain (`<- op X -> {CB} <- Y`) — see T04.
- COMPILED-PATH capture defect (evaa): post-hoc extraction resolved alternatives by first head-test match, so same-head XOR branches misbound (`@coll_ok` bound on fail traces). FIXED (2026-07-04): the compiled walker's Or/XOR handling (`_walk_matcher` in mch/compiler.py) is aligned with the v2 IR engine's `_match_or` — each branch's FULL positional walk is attempted against the remaining consumed slice under a cursor+captures savepoint, rolled back on failure, and the first fully-matching branch wins (with a legacy first-head-match fallback if none fully re-traces, so it never regresses below prior behavior). On the evaa fail fixtures the compiled Match now binds the FAIL-branch captures (`@coll_fail` / `@fail_excess`), matching the IR engine — proven by `tests/mch/integration/test_evaa_withdraw.py::test_evaa_*_binds_*` and the synthetic `tests/mch/test_compiler_behavioral.py::test_same_head_xor_*`. The consumed-set-driven builders (evaa / stonfi_v2 / dedust_swap) are kept as-is; capture-driven building on the compiled path is now available for same-head XOR shapes.
- Spec §14.3 marked known-idealized (wrong opcodes, wrong reject/destroy shapes, registration-order inversion); normative = specs/dedust_deposit.mch.
- TONStakersWithdraw delayed-arm NftMint deliberately NOT captured: capturing would consume it and diverge the consumed set (it stays its own nft_mint action); core reads it via `next_blocks` — faithful maximum.
- Sweep comparator: `block.data` may be a dataclass (port-wave blocks) — compared via shallow `vars()`; list fields compared as multisets because legacy `compact_connections()` set() rebuild makes order unstable across independently built trees (T21).
- Registration-order inversion (dedust_deposit): legacy pair runs BEFORE jetton_transfer matcher, mch port runs AFTER — deposit legs match prebuilt `btype jetton_transfer` with raw notify-chain fallback arm; consumed sets identical on all 8 fixtures.
