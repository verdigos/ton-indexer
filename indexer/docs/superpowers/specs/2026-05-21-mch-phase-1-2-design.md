# `.mch` DSL — Phase 1 closure + Phase 2 implementation

**Status:** Design (approved 2026-05-21).
**Scope:** Close out Phase 1 of the mch roadmap (verify the existing positional capture extractor, fill behavioral test gaps, fix surgical bugs if any) and implement Phase 2 (`build_block` output completeness). One coherent change set.
**Related:** `docs/superpowers/plans/2026-05-19-mch-future-roadmap.md` (parent roadmap).

---

## 1. Scope & goals

### What's in

1. **Phase 1 closure.** Audit and lock down the existing positional capture extractor (`_walk_matcher` in `indexer/events/mch/compiler.py:268-309`). Add behavioral tests for the edge cases that today are uncovered. Fix what fails surgically. Remove the stale comment in `tests/mch/integration/test_jetton_transfer.py:55-60`.
2. **Phase 2 implementation.** Make the synthesized `build_block` (compiler.py:189-197) return the full consumed-blocks list — `[produced, block, *other_blocks]` — so downstream serialization sees what was absorbed.
3. **Roadmap update.** In `docs/superpowers/plans/2026-05-19-mch-future-roadmap.md`, mark Phases 1 and 2 done; reflect that R009 was never wired in but the positional walker had already been written under v0.

### What's out

- Phase 3 list bindings under recursion. `RecursiveMatcher` stays opaque to the walker; captures inside it remain `None`.
- Phase 4 multi-hop parent chains. `C006_MULTI_HOP_PARENT_UNSUPPORTED` keeps rejecting `<- <-`.
- Phase 5 `ChildrenBlock` semantic rewrite. We will *test* what the current `ChildrenBlock -> OrMatcher` lowering binds, but we will not rewrite the lowering — that's a separate phase.
- Phases 6–11 (selectivity, ExclusiveOr heuristic, multi-file, codegen QoL, production wiring, corpus parity).
- Any C++ host or cross-language work.

### Success criteria

- All new edge-case capture-binding tests pass (or the walker is fixed surgically to make them pass).
- `build_block` returns the full consumed-blocks list; a behavioral test verifies the absorbed-blocks set equals the matcher's claimed match.
- Jetton-transfer integration test asserts on capture content (`m.transfer`, `m.internal`, `m.notify`) — not just block counts.
- No regression in the existing 89 mch tests; `tests/test_actions.py` baseline is unchanged.
- Roadmap document reflects the new state.

---

## 2. Phase 1 — Position-id capture extraction (closure)

### 2.1 Background

The roadmap (Phase 1, lines 33–55) described position-id capture extraction as the highest-value gap, anchored on removing the `R009_DUP_OPCODE_CAPTURE` resolver error. On audit:

- **R009 was never implemented.** `indexer/events/mch/resolver.py:82` has a comment "_reuse code; or define R009_" but the diagnostic is never raised. Duplicate-opcode captures are already accepted.
- **Approach (b) is already in `_walk_matcher`.** The walker mirrors the engine's `try_build` traversal order and binds captures by tree position via a `_mch_capture_name` tag set in `_lower_node` (compiler.py:509-513). Names function as position identifiers; no integer-id annotation is necessary.
- **Phase 1's two acceptance tests already pass:** `test_duplicate_opcode_chain_binds_distinct_blocks` and `test_captures_stable_across_recompiles` in `tests/mch/test_compiler_behavioral.py`.

What's missing is **coverage**, not implementation. Several pattern shapes that production matchers will rely on are not behaviorally tested, and at least two of them (parent-chain captures and children-block captures across sibling-orderings) have realistic bug surface.

### 2.2 Edge cases to cover

Walker traversal order (from compiler.py:268-309): test anchor → `child_matcher` → each entry of `children_matchers` in list order → `parent_matcher`. Engine's `try_build` (basic_matchers.py:32-62) builds `other_blocks` in the same order via `process_child_matcher` then `process_parent_matcher`. Each row below is one missing behavioral test.

| # | Pattern | Assertion | Implementation path | Risk |
|---|---|---|---|---|
| 2.2.1 | `op A @a -> ( op B @b \| op C @c )` | Firing branch's capture binds; other stays `None`. Both branches tested separately. | `OrMatcher` path: walker picks first sub whose `test_self` matches. | Low |
| 2.2.2 | `op A @a where pred_X` | Predicate False → no match; True → capture binds. | `_lower_node` combines `test_self` with predicate lambda. | Low |
| 2.2.3 | `op A @a <- op B @b` | After `try_build`, `m.b is block.previous_block`. | Walker recurses into `parent_matcher` *last*. Engine appends parent_matcher blocks to `other_blocks` last (basic_matchers.py:64-72). Cursor alignment must hold. | **Medium — never tested** |
| 2.2.4 | `btype jetton_transfer @j` as anchor | Anchor capture binds. | `BlockTypeMatcher` anchor with `_mch_capture_name` tag. | Low |
| 2.2.5 | `any @x` | Matches any block; capture binds. | `GenericMatcher(test_self_func=lambda _b: True)`. | Low |
| 2.2.6 | `op A @a -> btype jetton_transfer @j` | Mixed chain binds both. | Standard child chain. | Low |
| 2.2.7 | `op A @a -> { op B @b; op C @c }` against trees where the engine consumes siblings in different orders | Both children captures bind regardless of sibling order in the tree. | Engine's `process_children_matchers` (basic_matchers.py:89-118) scans `next_blocks` for the first match per child matcher; result order = children_matchers iteration order. Walker recurses in the same order. | **Medium — never tested** |

### 2.3 Decision rules for bug fixes

For each failing edge case:

- **First, predict via code reading whether the walker should handle it.** If it should and doesn't, the walker has a bug.
- **Surgical fix (<30 LOC) preferred** — patch `_walk_matcher` and ship.
- **If fix is >30 LOC** or requires changing the cursor model, **stop and document the limitation** as out-of-scope. Open a follow-on phase if needed. Do not refactor speculatively.

Anticipated outcomes:

- 2.2.1, 2.2.2, 2.2.4, 2.2.5, 2.2.6: expected to pass without code changes.
- 2.2.3 (parent-chain): cursor alignment with `parent_matcher` is uncertain. Walker visits parent last; engine extends `other_blocks` with parent's `try_build` result last. Should align, but verify.
- 2.2.7 (children sibling-order): if engine returns `[A, B, C]` for matchers `[B-matcher, C-matcher]` matching tree `[X, B, C]` where X comes first in `next_blocks` and is not absorbed, walker advancing cursor in matchers-order against `other_blocks` should still work — `other_blocks` only contains B and C, not X. Should align.

### 2.4 Stale-comment cleanup

`tests/mch/integration/test_jetton_transfer.py:54-60` claims that the v0 extractor returns `None` for symbolic-opcode captures. That was true under an earlier `_make_extractor` implementation; the current `_walk_matcher` uses `m.test_self`, which is independent of how the opcode was named in source. **Action:** delete the comment and make the builder assert that `m.transfer`, `m.internal`, `m.notify` are populated when expected.

---

## 3. Phase 2 — `build_block` output completeness

### 3.1 Current behavior

`compiler.py:189-197`:

```python
async def build_block(self, block, other_blocks):
    kwargs = _extract_captures(self, block, other_blocks, type(self)._mch_capture_names)
    m = type(self)._mch_match_cls(**kwargs)
    produced = await builder(m)
    out_blocks = [produced] if produced is not None else []
    return out_blocks
```

This drops `block` (the anchor) and `other_blocks` (the children/parent absorbed blocks) from the return. The base class default (basic_matchers.py:120-121) is `return [block] + other_blocks` — the contract every hand-written matcher honors. The DSL silently breaks it.

### 3.2 Proposed change

```python
async def build_block(self, block, other_blocks):
    kwargs = _extract_captures(self, block, other_blocks, type(self)._mch_capture_names)
    m = type(self)._mch_match_cls(**kwargs)
    produced = await builder(m)
    if produced is None:
        return []
    produced.merge_blocks([block] + other_blocks)
    return [produced]
```

**Amendment (added during plan writing, revised 2026-05-21 after independent review):** Verified §3.4's parent-linkage concern. The engine does NOT auto-wire `block.parent` — hand-written matchers do it themselves via `produced.merge_blocks([block] + other_blocks)` inside `build_block` (see `indexer/events/blocks/elections.py:46`, `indexer/events/blocks/cocoon.py:210` and 13+ other call sites). Without this call, the DSL never marks consumed blocks absorbed, and downstream matchers re-process them. §3.4's "if the engine handles it uniformly, no extra work" alternative is FALSE — it does not.

**Return shape decision:** Mirror hand-written matchers exactly — return `[produced]`, not `[produced, block, *other_blocks]`. Independent review converged on this: when a DSL matcher is composed as a `child_matcher` of another matcher, the parent's `process_child_matcher` does `blocks.extend(r)` — passing already-absorbed children up would pollute the parent's `other_blocks`. Hand-written matchers like `ElectionDepositStakeBlockMatcher.build_block` (`elections.py:46-47`) return `[new_block]` only; `merge_blocks` does the absorption side-effect, the return list advertises only the logically-new block.

Phase 2 therefore requires both the `merge_blocks` call AND returning `[produced]` (single-element).

### 3.3 `produced is None` semantics

Current convention: builder returns `None` to signal "match rejected." Keep returning `[]` so the engine doesn't absorb anything. The auto-append code path checks `len(auto_append_opcodes) > 0` then iterates `block.next_blocks` and calls `r.append(...)` — if we return `[]` and the engine appends excess/bounce, the engine would absorb excess blocks for a rejected match. This is actually a latent bug regardless of Phase 2, but verify the post-build auto-append doesn't fire when the produced list is empty. If it does, return `[]` and have the matcher also signal rejection by returning `None` from `try_build` — needs inspection. **Will verify against `BlockMatcher.try_build` (basic_matchers.py:42-60) during implementation.** If a fix is needed, it stays inside `build_block` (return `None` instead of `[]` when builder rejects, if that's what the engine expects).

### 3.4 Parent-linkage concern

Hand-written matchers' results become the new "parent" of absorbed blocks (the engine wires `absorbed.parent = produced` somewhere — needs verification against `event_processing.py`). If the DSL's `produced` block isn't wired the same way, downstream Action serialization could mis-attribute. **Verification step before implementation:** read the absorb code in `event_processing.py` or wherever `try_build`'s return is consumed; confirm parent-linkage is applied uniformly to whatever the matcher returns. If it's not uniform — DSL needs to do the wiring explicitly. If it is uniform, no extra work.

---

## 4. Tests & regression strategy

### 4.1 New tests

In `tests/mch/test_compiler_behavioral.py`:

| Test name | Phase | Asserts |
|---|---|---|
| `test_alternative_branch_capture_firing_branch_binds` | 1 | When `(op B @b \| op C @c)`'s B-branch fires: `m.b` is the B block, `m.c is None`. Symmetric for C. |
| `test_where_predicate_filters_then_binds_capture` | 1 | Predicate-False block doesn't match; True-block matches and capture binds. |
| `test_parent_chain_capture_binding` | 1 | `op A @a <- op B @b`: against `B(child=A)` tree, `m.a is A_block`, `m.b is B_block`. |
| `test_btype_anchor_capture_binding` | 1 | Anchor is `btype X @j`; capture binds. |
| `test_any_head_capture_binding` | 1 | `any @x` captures whatever block hit. |
| `test_mixed_op_and_btype_chain_capture_binding` | 1 | Chain mixing OpHead and BTypeHead binds both. |
| `test_children_block_captures_bind_regardless_of_sibling_order` | 1 | `op A @a -> { op B @b; op C @c }` against trees `[B, C]` and `[C, B]` and `[X, B, C]` (X unrelated). All three bind correctly. |
| `test_build_block_returns_full_consumed_set` | 2 | After `try_build` on chain `A -> B -> C`, return list is `[produced, A_block, B_block, C_block]` (modulo excess/bounce which are auto-appended). |
| `test_build_block_empty_when_builder_returns_none` | 2 | Builder returning `None` yields `[]` from `try_build`; no excess/bounce leak. |

### 4.2 Integration test updates

**`tests/mch/integration/test_jetton_transfer.py`:**

- Remove stale comment lines 55-60.
- Builder asserts captures are populated as expected: `m.transfer.opcode == JettonTransfer.opcode`; same shape check for `m.internal` and `m.notify` (with `m.notify` allowed to be `None` since it's `maybe`-wrapped).
- Existing structural-count assertion stays.

**`tests/mch/integration/test_stonfi_v2_swap.py`:**

- Has an equivalent stale comment at lines 83-85 ("we don't depend on m.swap being populated"). Remove it.
- The pattern is `op StonfiV2Swap @swap -> op StonfiV2PayTo @payout -> $next_hops`. Both `@swap` and `@payout` are outside the recursive rule, so the Phase 1 walker MUST bind them. The builder asserts `m.swap.opcode == 0x6664de2a` and `m.payout.opcode == 0x657b54f5`.
- Captures inside the `next_hops` recursive rule (none today, but the rule body could grow them) remain `None` — that's Phase 3's job and is explicit in §6.

### 4.3 Regression checks

| Step | Command | Expected |
|---|---|---|
| After each Phase 1 test addition | `python -m pytest tests/mch/ -v` | Baseline (89) + new tests, all green. |
| After Phase 2 build_block change | `python -m pytest tests/mch/ -v` | All green including new Phase 2 tests. |
| After Phase 2 build_block change | `python -m pytest tests/test_actions.py -v` | Identical pass/fail to baseline (DSL not wired into production yet — this is a smoke check). |
| Final | `python -m pytest tests/ -v` | No regression vs baseline. |

---

## 5. Risks & rollback

### 5.1 Risks

1. **Parent-chain capture test (2.2.3) reveals walker doesn't reach `parent_matcher` correctly.** Walker visits parent last; engine appends parent's blocks to `other_blocks` last. Should align — but if cursor model is off-by-N, fix surgically in `_walk_matcher`.
2. **Children-block sibling-order test (2.2.7) fails.** If walker's in-order recursion doesn't tolerate engine's matcher-priority consumption order, fall back to per-matcher search (each sub-matcher tests against each unconsumed block from `other_blocks`). ~30 LOC if needed.
3. **Phase 2 order change breaks integration tests.** DSL matchers aren't in production (Phase 10), so blast radius is contained to `tests/mch/integration/*`. Low risk.
4. **`produced is None` + auto-append latent bug.** Discovered during Section 3.3 inspection. If real, fix it inside `build_block` by signaling rejection upstream — keep change minimal.
5. **Parent-linkage on `produced` block.** Section 3.4 concern. If the DSL needs to explicitly wire `absorbed.parent = produced`, scope grows by ~20 LOC and one test. If the engine handles it uniformly, no change.

### 5.2 Rollback

- All changes confined to `indexer/events/mch/compiler.py`, `tests/mch/**`, and `docs/superpowers/plans/2026-05-19-mch-future-roadmap.md`.
- No production code edited; DSL matchers are not wired into the classifier yet.
- Per user instruction: do not commit. Working-tree changes can be reverted with `git checkout -- <paths>` at any point.

### 5.3 Branch note

Current branch is `fix/classifier-gating`, which is unrelated to this work. Branch management is left to the user — no commits will be made on this branch as part of this work.

---

## 6. Out of scope (explicit)

- Phase 3 list bindings under recursion. The walker treats `RecursiveMatcher` as opaque; captures inside recursion stay `None`. To be addressed in a future phase.
- Phase 4 multi-hop parent chains. `C006` remains active.
- Phase 5 `ChildrenBlock` semantic rewrite. The lowering from `ChildrenBlock` to `OrMatcher` in `_lower_atom` is known to be semantically wrong for non-anchor positions, but fixing it is a separate phase. We do *not* touch `_lower_atom`'s ChildrenBlock branch.
- Phases 6–11 (selectivity, ExclusiveOr heuristic, multi-file, codegen QoL, production wiring, corpus parity).
- C++ host work.
- Stitch declarations and cross-trace mechanics.

---

## 7. Definition of done

1. All tests listed in §4.1 are present and pass.
2. The Phase 2 change is in place in `compiler.py`; the integration test asserts capture content; baseline regression check is green.
3. Stale comment at `tests/mch/integration/test_jetton_transfer.py:55-60` is removed.
4. Roadmap document (`docs/superpowers/plans/2026-05-19-mch-future-roadmap.md`) is updated to mark Phases 1 and 2 done with a brief note on what was already in v0 vs newly added.
5. `python -m pytest tests/mch/ -v` reports baseline-plus-new-tests, all green.
6. `python -m pytest tests/test_actions.py -v` is identical to baseline.
7. No commits are made. Working-tree diff is reported to the user.
