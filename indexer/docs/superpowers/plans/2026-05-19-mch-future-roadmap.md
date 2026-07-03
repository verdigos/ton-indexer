# `.mch` DSL — Roadmap to a Fully Capable Compiler

> **SUPERSEDED (2026-07-03):** replaced by `docs/mch-roadmap.md` (task list, spec v0.2 milestones M1–M5). Phases 1–5 and 7 below are implemented (commits 5ddd0f2a, 4d727265); Phases 6, 8–11 are folded into the new task list. Kept for historical context only.

**Status:** Roadmap (not a step-by-step implementation plan).
**Scope:** Everything needed to make the Python compiler able to express every existing hand-written matcher under `indexer/events/blocks/` — EXCEPT the stitch / cross-trace mechanics, which are explicitly out of scope per user instruction.

This document lists the gaps between v0 and "fully capable", grouped by theme. Each gap states what's missing today, why it matters, the proposed approach, and a definition of done. Phases are ordered so that each phase produces a tested, useful capability and depends only on earlier phases.

The current v0 (86 mch tests passing) is the starting line.

---

## Reference: what the production matchers actually use

A quick census of `indexer/events/blocks/*.py` (the matchers we ultimately need to be able to replace) shows where the v0 DSL has gaps:

| Feature | Production usage | v0 DSL support |
|---|---|---|
| Linear `op X -> op Y` chains | universal | ✅ works |
| `children_matchers` | 10 protocols, ~58 usages | ⚠️ works for anchor's direct children only |
| `parent_matcher` (single hop) | 14 protocols, ~125 usages | ⚠️ works only when anchor is in tail position |
| `parent_matcher` (multi-hop) | a handful | ❌ rejected with C006 |
| `RecursiveMatcher` (direct self-ref) | 3 protocols | ✅ works for the `step \| maybe $self` shape |
| `RecursiveMatcher` (indirect / mutual) | none today | ❌ rejected with C005 (keep blocked) |
| `find_call_contract` (i.e. capture-by-opcode) | 7 protocols | ⚠️ works but loses precision with duplicate opcodes (rejected with R009) |
| `where` predicate clauses | several | ✅ works |
| `include_excess` / `include_bounces` | universal default | ⚠️ flag passes through but builder output may be incomplete |
| Capture lists under recursion (`list[T]`) | n/a in production (handcoded loops) | ❌ codegen emits the type but extractor never populates |

The two highest-value items are precise capture extraction (Phase 1) and complete `build_block` output (Phase 2). Together they unlock the bulk of the existing protocol catalog.

---

## Phase 1 — Position-id capture extraction

**Status:** ✅ Done (2026-05-21). The positional walker (`_walk_matcher` in `indexer/events/mch/compiler.py`) was already implemented during v0 — it mirrors the engine's traversal order and binds captures by `_mch_capture_name` tag (semantically equivalent to a "position id"). R009 was never wired into the resolver; duplicate-opcode captures have always been accepted. Phase 1 closure added behavioral tests for alternative branches, where-predicate, parent chain, btype anchor, any head, mixed op+btype chains, and children-block sibling order — surfaced three real compiler bugs which were fixed surgically (anchor where-predicate not applied; parent chain not lowered when anchor is the sequence head; PARENT-edge atoms wrongly included in the child chain). See `docs/superpowers/specs/2026-05-21-mch-phase-1-2-design.md` and `docs/superpowers/plans/2026-05-21-mch-phase-1-2-implementation.md`.

**Gap.** Today's `_make_extractor` searches `[anchor] + other_blocks` for the first block whose opcode/btype matches the capture's head. This:
- Cannot tell two `op X` captures apart (the resolver rejects them with `R009_DUP_OPCODE_CAPTURE`, which prevents a real swap-chain pattern like `op A @first -> op A @second` from being written).
- Picks the first matching block by document order, not by the position in the pattern the capture was bound at. In a tree with multiple siblings, the wrong sibling can win.

**Why it matters.** Every multi-hop pattern that reuses an opcode (swap chains, deposit/withdraw legs, payout chains) is currently inexpressible. This is the single biggest blocker to a fully capable compiler.

**Proposed approach.**
1. During lowering, assign each AST `Node` that owns a capture a stable integer "position id" reflecting the depth-first lowering walk order.
2. Annotate each lowered `BlockMatcher` instance with the position ids of any captures it owns (a new attribute, e.g. `_mch_capture_positions: tuple[int, ...]`).
3. Teach the synthesized `build_block` to walk the actual matched block set (`[block] + other_blocks`) in the engine's documented order and pair each consumed block with the position id whose matcher consumed it. This requires either:
   - **(a)** Extending `BlockMatcher.try_build` to return ordered position metadata alongside the block list — invasive, touches engine code.
   - **(b)** Re-walking the matched tree at build time and re-running per-position predicates against the consumed set — slightly slower but engine-pristine.
4. Remove `R009_DUP_OPCODE_CAPTURE` (or downgrade to a warning) once position-id extraction lands.

**Definition of done.**
- A pattern `op A @first -> op A @second` resolves cleanly, compiles, and runs against a synthetic block tree where the builder receives `m.first` and `m.second` as the correct distinct blocks.
- All existing 86 mch tests stay green.
- A new test verifies position-id stability across recompilations of the same matcher.

**Complexity.** Medium. Approach (b) is preferred — no engine changes; ~300 LOC including tests.

---

## Phase 2 — `build_block` output completeness

**Status:** ✅ Done (2026-05-21). The synthesized `build_block` now calls `produced.merge_blocks([block] + other_blocks)` to wire parent linkage (mirroring all hand-written matchers — e.g. `elections.py:46`, `cocoon.py:210`) and returns `[produced]` only (matching the hand-written convention). Additionally, a synthesized `try_build` override prevents the engine's auto-append step from leaking excess/bounce blocks into a rejected match's result. The original roadmap prescription (returning the full consumed list) was revised after independent review — see `docs/superpowers/specs/2026-05-21-mch-phase-1-2-design.md` §3.2 amendment.

**Gap.** The v0 synthesized `build_block` returns `[produced]` only. The engine's base `BlockMatcher.try_build` then auto-appends excess/bounce blocks. But it does NOT include the originally-matched anchor block or the blocks consumed by child / children matchers in the output list. Compare to the existing hand-written `BlockMatcher.build_block` default at `basic_matchers.py:120`:

```python
async def build_block(self, block, other_blocks):
    return [block] + other_blocks
```

That contract — return all consumed blocks so downstream serialization knows what was absorbed — is what every hand-written matcher honors. The DSL-compiled subclass overrides it to return only `[produced]`, breaking the contract.

**Why it matters.** Without the consumed-blocks list, downstream serialization can't tell which transactions were absorbed by a match versus left for other matchers. In production this leaks into `Action` row generation. Today our integration tests don't trip on this because they compare counts of `JettonTransferBlock` instances, not the full block tree.

**Proposed approach.**
1. The synthesized `build_block` returns `[produced, block, *other_blocks]` (or equivalent: the produced block first, then the originally-matched blocks).
2. Honor `include_excess` / `include_bounces` exactly the way the base class does — they're already passed through to the base's auto-append step, so this should "just work" once the consumed list is correct.
3. If `produced is None` (current builder convention for "I rejected the match"), still return `[]` — but consider whether returning the consumed-blocks list without `produced` is a cleaner contract.

**Definition of done.**
- An integration test asserts that after `try_build` returns, the matched block set in the tree is exactly the set of blocks the builder claims to have consumed.
- Wider regression: rerun `tests/test_actions.py` (123 cases) to confirm no production-action behavior diverges if DSL matchers were swapped in.

**Complexity.** Small. ~50 LOC.

---

## Phase 3 — List bindings under recursion

**Status:** Done (2026-05-21). The walker now reads `_MchRecursiveMatcher._mch_iter_blocks` (per-iteration consumed blocks recorded BEFORE the engine's `set()` dedup) and populates `list_binding` captures via per-iter walks. `_lower_recursive_rule` strips trailing `maybe $self` from step branches and passes `exit_matcher=None` when no pure-exit branches exist — fixes the latent v0 "always-one-iteration" bug. New `C007_CAPTURE_AFTER_RECURSION` diagnostic rejects patterns that would silently lose captures placed after a recursive-rule reference. See `docs/superpowers/specs/2026-05-21-mch-phase-3-4-design.md`.

**Gap.** Spec §7.2 says captures inside a self-referential rule expose as `list[T]`, parallel-aligned across peer captures. Codegen already emits the type. The runtime extractor doesn't populate the list — it just runs the single-block search.

**Why it matters.** The `stonfi_v2_swap` example in `docs/mch-spec.md` §14.2 explicitly relies on this:

```
swap:    list[CallContractBlock]  # one per hop
payout:  list[CallContractBlock]  # parallel to swap
out_transfer: Block | None        # only set on the terminating hop
```

Without proper list bindings, builders can't introspect intermediate swap hops — they see only one. Production matchers work around this with hand-coded iteration; we can't replicate that without lists.

**Proposed approach.**
1. Track during the recursion-strategy walk how many times the recursive step fires.
2. Captures inside the step body collect into per-iteration lists, keyed by position id.
3. The Match dataclass field is populated with the list in trace order (matching the engine's `RecursiveMatcher.process_child_matcher` traversal order).

**Definition of done.**
- A synthetic 3-hop recursion test asserts `len(m.swap) == 3`, `len(m.payout) == 3`, and `m.swap[i]` / `m.payout[i]` are paired correctly.
- The stonfi v2 integration test is upgraded to verify `len(m.swap)` matches the trace's actual hop count.

**Complexity.** Medium. Depends on Phase 1's position-id machinery. ~150 LOC.

---

## Phase 4 — Multi-hop parent chains

**Status:** Done (2026-05-21). `_lower_parent_chain_from` rewrites to walk consecutive PARENT edges from the anchor outward, building a `parent_matcher.parent_matcher.…` chain. Handles anchor-is-head and both anchor-in-tail variants (B.1 anchor-edge=PARENT and B.2 anchor-edge=CHILD). `_check_no_multi_hop_parent` and the `C006_MULTI_HOP_PARENT_UNSUPPORTED` diagnostic removed. Walker recursion into `parent_matcher` unchanged from Phase 1/2 — handles multi-hop naturally. See `docs/superpowers/specs/2026-05-21-mch-phase-3-4-design.md`.

**Gap.** `_lower_parent_chain_from` lowers only the immediate predecessor of the anchor and emits `C006_MULTI_HOP_PARENT_UNSUPPORTED` for any pattern with two consecutive `<-` edges. Production has matchers like NFT auction flows that walk upward two or three levels.

**Why it matters.** Any matcher whose anchor isn't the topmost interesting block needs to express upstream context. Workarounds exist (move the anchor higher, use a `pred` predicate that itself inspects ancestors), but they fight the DSL's intent.

**Proposed approach.**
1. Recurse `_lower_parent_chain_from` to follow chains of `<-` edges, building a `parent_matcher.parent_matcher.parent_matcher` chain on the engine side.
2. Decide on the left-associativity semantics carefully (the spec docs `A -> B <- C <- D` as "D is C's parent, C is B's parent" — meaning chained, not flat). Match that interpretation.
3. Remove `C006`.

**Definition of done.**
- A pattern `op A @a <- op B @b <- op C @c` compiles, and against a synthetic tree `A(parent=B(parent=C))` the matcher fires with `m.a`, `m.b`, `m.c` correctly bound.
- The existing C006 test (`test_multi_hop_parent_chain_rejected`) is converted to a positive test.

**Complexity.** Small-medium. ~100 LOC including tests.

---

## Phase 5 — `ChildrenBlock` in non-anchor positions

**Status:** ✅ Done (2026-05-21). `_lower_chain_with_children_attach` helper added to `compiler.py`; `_lower_atom(Sequence)` rewritten to use it; `_lower_atom(ChildrenBlock)` and `_lower_atom(Maybe(ChildrenBlock))` emit `C008_CHILDREN_BLOCK_BARE` when bare; `_lower_child_chain_from` updated to use helper for mid-chain and rest-of-chain atoms; `_lower_parent_chain_from` updated to attach trailing ChildrenBlocks to the outermost parent matcher. 12 new behavioral tests. `basic_matchers.py` untouched.

**Gap.** Reviewer issue #13: `_lower_atom` lowers a `ChildrenBlock` to `OrMatcher`, which means "match any one". The DSL intent for `{ A; B }` is "match all of these as parallel children". The wrong semantics only manifest when a children block appears outside the anchor's direct child slot (e.g. inside a rule body or alternative branch).

**Why it matters.** Several production matchers use children blocks at arbitrary depths (dedust deposit, jvault). The compiler currently produces silently-wrong matchers for such patterns.

**Proposed approach.**
1. When `_lower_atom` sees a `ChildrenBlock`, synthesize a wrapper matcher (a tiny `BlockMatcher` subclass with `test_self` returning True and `children_matchers` set to the lowered items) — semantically equivalent to "match a parent whose children include these".
2. The wrapper's anchor block is whatever the caller is binding; the children matchers attach to it.
3. Document that v0's "ChildrenBlock = OrMatcher" was wrong; positive tests guard the new semantics.

**Definition of done.**
- Pattern `op A @a -> ( op X -> { op B @b; op C @c } )` compiles and matches a tree where A's child is an X whose children include B and C.
- Negative test confirms an unrelated child between A and X doesn't satisfy the children block.

**Complexity.** Medium. ~150 LOC.

---

## Phase 6 — Optional `entry` via selectivity statistics

**Gap.** v0 requires `entry @X` on every matcher (deviation from spec §10.2). The spec wants selectivity-based auto-anchor selection using `opcode_freq.json` / `btype_freq.json` corpus statistics, with `entry` as an explicit override.

**Why it matters.** Lower author burden. Production matchers don't have to think about anchor choice. Also enables the rarer-opcode-wins optimization that makes the engine faster on traces dominated by common opcodes.

**Proposed approach.**
1. Add `dev_utils/generate_opcode_freq.py` that scans `tests/traces/*.lz4`, counts opcodes and btypes, writes the two JSON files.
2. Update the resolver to load these files (location configurable via env or a default) and pick a default anchor when `entry` is absent.
3. Implement the ranking rules from spec §10.2:
   - Walk the pattern AST, collect concrete `op`/`btype` candidates outside `maybe` / alternatives / recursive rules.
   - Rank by frequency (rarer = better).
   - Break ties by source position.
   - Warn `W201_NO_SELECTIVITY_STATS` if stats are missing for a candidate; fall back to syntactic-first.
4. Downgrade `R008_ENTRY_REQUIRED` to a warning when stats are available; remove it entirely once trustworthy stats ship.

**Definition of done.**
- Stats files are present (or can be regenerated by a make target).
- A matcher without `entry` compiles, and the chosen anchor is the rarest concrete op/btype in the pattern.
- Explicit `entry` still overrides.

**Complexity.** Medium. ~250 LOC including the corpus scanner.

---

## Phase 7 — `ExclusiveOrMatcher` heuristic

**Status:** ✅ Done (2026-05-21). `CARET = "^"` token added to `tokens.py` and `lexer.py`. `Alternative.exclusive: bool` field added to `ast_.py`. Parser updated: `_parse_pattern_expr` peek-and-remember separator, P010 on mix. Compiler updated: `_branches_eligible_for_exclusive_heuristic` + `_check_redundant_exclusive_heads` (W202) helpers; `_lower_atom(Alternative)` dispatches to `ExclusiveOrMatcher` for explicit `^` or heuristic-eligible `|`. 16 new tests (lexer, parser, structural, behavioral). `basic_matchers.py` untouched.

**Gap.** Alternatives always lower to `OrMatcher`. The spec mentions `ExclusiveOrMatcher` as a possible optimization "if all branches' anchors are concrete and mutually exclusive". v0 punts.

**Why it matters.** `ExclusiveOrMatcher` is faster for the engine because `test_self` rejects early if more than one branch claims the block. It also catches authoring mistakes where the user intended exclusive branches.

**Proposed approach.**
1. During lowering of `Alternative`, inspect each branch's anchor node head.
2. If all branches are concrete (`op` or `btype` heads) and pairwise distinct, lower to `ExclusiveOrMatcher`.
3. Otherwise lower to `OrMatcher`.

**Definition of done.**
- A pattern `( op A @a | op B @b )` (both concrete, distinct) lowers to `ExclusiveOrMatcher`.
- A pattern `( op A @a | pred X @y )` lowers to `OrMatcher`.
- Behavior parity verified via synthetic tests.

**Complexity.** Small. ~50 LOC.

---

## Phase 8 — Multi-file `.mch` namespace

**Gap.** Spec §2 says multiple `.mch` files in the same directory "share a single logical namespace; cross-file references are allowed". `compile_dir` currently compiles each file independently, so a `rule` declared in `a.mch` can't be `$ref`-ed from `b.mch`.

**Why it matters.** Real protocol catalogs split across files for organization. Today every file has to be self-contained, including re-declaring shared opcodes — friction for both authoring and review.

**Proposed approach.**
1. Add `compile_namespace(dir, registries)` (or extend `compile_dir`) that:
   - Lexes + parses every `.mch` in the dir into per-file ASTs.
   - Merges declarations into a single virtual `File` (or runs the resolver in a namespace-aware mode that pools opcodes/predicates/rules across files).
   - Validates: duplicate opcode declarations across files must agree on value (per spec §5.1); duplicate matcher names are an error.
2. Each file's matchers compile against the merged symbol table.
3. Diagnostics still report the file:line:col of the original token (already handled by `Span.path`).

**Definition of done.**
- Two `.mch` files where `b.mch` references a `rule` from `a.mch` compile cleanly.
- An opcode declared in both files with conflicting values produces a single, clearly-attributed error.

**Complexity.** Medium. ~200 LOC plus tests.

---

## Phase 9 — Codegen quality of life

**Gap.** Three concrete items:
- `btype` captures emit `Block` as the field type. The actual class is known if the host registry has it bound. Codegen should consult `registries.block_types` and emit the precise type when available.
- No `.pyi` stub generation. For projects that prefer to keep `generated/` out of source control, a `.pyi` would still give IDE support.
- No CLI wrapper. Today authors have to hand-roll a script (the usage guide ships a template).

**Proposed approach.**
1. Plumb `registries.block_types` into `_capture_type_str`. Add an import for the precise class when known; fall back to `Block` otherwise.
2. Add a `--stubs` mode to codegen that emits `.pyi` instead of `.py`. Same content, file-extension different.
3. Add `dev_utils/mch_codegen.py` (or `python -m indexer.events.mch.codegen`) as a CLI entry point. Args: `--specs DIR --out DIR --stubs`.

**Definition of done.**
- A `btype jetton_transfer @j` capture in a spec compiles to a Match dataclass with `j: JettonTransferBlock` (not `j: Block`) when the host has registered `JettonTransferBlock`.
- `python -m indexer.events.mch.codegen --specs indexer/events/mch/specs --out indexer/events/mch/generated` is the documented one-liner.

**Complexity.** Small-medium. ~150 LOC including the CLI.

---

## Phase 10 — Production wiring under a feature flag

**Gap.** The module is standalone. Even with everything above done, the engine still uses hand-written matchers in production. Nothing in `event_processing.py` consults the DSL.

**Why it matters.** This is the integration that makes the work pay rent. Until DSL matchers replace hand-written ones (per-protocol, incrementally), the DSL is dead code in production.

**Proposed approach.**
1. Pick one hand-written matcher to start with (recommend `JettonTransferBlockMatcher` — simplest, well-tested, the DSL spec already exists at `specs/jetton_transfer.mch`).
2. Add an env var `TON_INDEXER_MCH_MATCHERS=jetton_transfer` (comma-separated list).
3. In `event_processing.py`, after loading hand-written matchers, compile the listed `.mch` files and replace the corresponding hand-written matcher in the global `matchers` list.
4. Run the full `tests/test_actions.py` suite with the flag enabled; require parity.
5. Stable matcher-by-matcher rollout.

**Definition of done.**
- With `TON_INDEXER_MCH_MATCHERS=jetton_transfer` set, `tests/test_actions.py::TestJettonTransfers` passes identically.
- A documented procedure for promoting additional matchers.

**Complexity.** Medium — the risk is in the production semantics, not the code volume. ~200 LOC plus iteration.

---

## Phase 11 — Test corpus parity

**Gap.** Today's integration tests cover jetton_transfer and stonfi_v2_swap on one trace each. We don't know how broadly the DSL covers the production matcher catalog.

**Why it matters.** Without a parity test against the YAML test corpus, we can't claim "fully capable" with confidence. Each protocol that gets a DSL spec needs the same kind of test the hand-written matcher already has.

**Proposed approach.**
1. Pick the top ~10 hand-written matchers (by usage). For each: write the `.mch` spec, write an integration test that runs both the DSL-compiled and hand-written matchers against the protocol's existing YAML test cases, assert structural parity.
2. Promote each as a Phase 10 rollout candidate.

Order (rough priority):
- `JettonTransferBlockMatcher` (foundation)
- `JettonBurnBlockMatcher`
- `JettonMintBlockMatcher`
- `PTonTransferMatcher`
- `StonfiSwapBlockMatcher` (V1)
- `StonfiV2SwapBlockMatcher` (already covered)
- `DedustSwapBlockMatcher`
- `NftTransferBlockMatcher`
- `NftPurchaseBlockMatcher`
- `ElectionDepositStakeBlockMatcher`

Skip anything that uses cross-trace stitching (Dedust deposit/withdraw, etc.) — that's the explicitly excluded set.

**Definition of done.**
- 10+ DSL specs in `indexer/events/mch/specs/`.
- 10+ integration tests, each comparing DSL vs hand-written on real traces.
- Documented per-matcher parity (counts match, structural shape matches).

**Complexity.** Mostly mechanical once Phases 1–5 are done. Per-matcher cost is small but volume adds up. ~50 LOC + 1 YAML-derived test per matcher.

---

## Recommended execution order and dependencies

```
✅ Phase 1 (position-id extraction)
  └─ ✅ Phase 2 (build_block output)
       └─ ✅ Phase 3 (list bindings)
       └─ ✅ Phase 4 (multi-hop parent)
       └─ ✅ Phase 5 (children-block semantics)
            └─ ✅ Phase 7 (ExclusiveOr heuristic + `^` syntax)
            └─ Phase 11 (corpus parity)
                 └─ Phase 10 (production wiring)

Phase 6 (selectivity)  — independent; do anytime
Phase 8 (multi-file)   — independent; do anytime before Phase 11
Phase 9 (codegen QoL)  — independent; do anytime
```

**Critical path** to "fully capable" + production wiring: ~~1 → 2 → 4 → 5 → 7~~ → 11 → 10. Phases 1–5 + 7 done 2026-05-21.
**Phases that can run in parallel** with the critical path: 6, 8, 9.

If forced to pick a minimum viable next step, **Phase 1 is the single highest-value change** — it removes the R009 restriction that prevents writing realistic swap-chain patterns. Phase 2 cleans up the build_block contract so DSL matchers don't quietly produce wrong output downstream.

---

## Out of scope (per user instruction)

- **Stitch declarations and runtime.** Parser accepts; resolver warns; compiler skips. Not implementing the cross-trace pending-action table, the TTL machinery, or the `merge` callback contract.
- **C++ host (§11.2) and cross-language fidelity (§11.3).** Python only.

These exclusions are durable across all phases above.

---

## Definition of done (the whole roadmap)

The compiler is "fully capable" when:

1. Every hand-written matcher in `indexer/events/blocks/*.py` that doesn't use cross-trace stitching has an equivalent `.mch` spec in `indexer/events/mch/specs/`.
2. Each spec has a parity integration test against its YAML corpus.
3. At least one matcher (jetton_transfer) is shipped under the production feature flag and passes the full `tests/test_actions.py` suite with the flag enabled.
4. Codegen produces precisely-typed Match dataclasses (no `Block` fallback when the host registry knows the class).
5. Multi-file specs work; opcode/predicate/rule namespaces are pooled across `.mch` files in a directory.
6. The R009 / C005 / C006 / C007 / C001 diagnostics are either resolved (~~R009~~ confirmed never implemented; ~~C006~~ resolved 2026-05-21; C007 added 2026-05-21 to reject captures after recursion) or remain as documented unsupported features (C005 for indirect recursion, C001 for unsupported recursion shapes).

When all six are true, the compiler can replace any non-stitch hand-written matcher one-by-one without regression.
