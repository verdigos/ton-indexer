# mch DSL — Phase 1 closure + Phase 2 implementation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **No commits.** Per user preference, **do not run `git commit` or `git push` at any point**, regardless of standard plan templates. Save and stage edits in the working tree only; the user will commit when ready. Treat any "Commit" step in standard task templates as "Stop and verify the diff; do not run git commit."

**Goal:** Close out roadmap Phase 1 (verify positional capture extraction covers all pattern shapes) and implement Phase 2 (synthesized `build_block` absorbs consumed blocks via `merge_blocks` and returns the full consumed list).

**Architecture:** The existing `_walk_matcher` in `indexer/events/mch/compiler.py` already implements approach (b) from the roadmap (re-walking the matched tree at build time). We add behavioral tests for the seven untested pattern shapes; fix the walker surgically (<30 LOC) if any test fails. We then change the synthesized `build_block` to call `produced.merge_blocks([block] + other_blocks)` (the absorption mechanism used by all hand-written matchers — discovered during plan writing, see spec §3.2 amendment) and return `[produced, block, *other_blocks]`.

**Tech Stack:** Python 3.10+, pytest with `pytest-asyncio`, SQLAlchemy 1.4 (unused for these tests), virtual env at `S:\Dev\H-dump\tonscan-indexator\venv0`. Tests run from `S:\Dev\ton-indexer-clear\indexer`.

**Spec:** `docs/superpowers/specs/2026-05-21-mch-phase-1-2-design.md`.

**Baseline:** 89 mch tests passing. Final target: 89 + 9 new tests = 98 mch tests passing, plus updated integration tests still passing.

---

## File map

| Path | Action | Responsibility |
|---|---|---|
| `tests/mch/test_compiler_behavioral.py` | Modify | Add 9 new behavioral tests (7 Phase 1 edge cases + 2 Phase 2 tests). |
| `tests/mch/integration/test_jetton_transfer.py` | Modify | Remove stale comment, assert on capture content. |
| `tests/mch/integration/test_stonfi_v2_swap.py` | Modify | Remove stale comment, assert on capture content. |
| `indexer/events/mch/compiler.py` | Modify | Phase 2: add `merge_blocks` call + change return shape in synthesized `build_block`. Possibly: small fix to `_walk_matcher` if an edge-case test fails. |
| `docs/superpowers/plans/2026-05-19-mch-future-roadmap.md` | Modify | Mark Phases 1 and 2 done; note R009 was never wired in. |

No new files. No production code paths (outside the DSL module) are touched.

---

## Conventions for every test

- The test framework is `pytest-asyncio`. Use `@pytest.mark.asyncio` on every async test.
- Use `build_block_tree` fixture from `tests/mch/conftest.py` (BlockSpec-based; sets `previous_block` on children, leaves the top block with `previous_block=None`).
- Use `_registries_with(opcodes, builder_name, produces)` helper already defined in `tests/mch/test_compiler_behavioral.py:11-21`. It returns `(reg, captured_matches)`: the registry has the opcodes set and a builder registered that captures match objects into the list.
- Test scaffolding pattern (from `test_compiler_behavioral.py`):

```python
src = """
opcode A = 0x1
matcher m {
    produces MB
    build    build_m
    entry    @a

    op A @a
}
"""
reg, captured = _registries_with({"A": 0x1}, "build_m", "MB")
compiled = compile_text(src, "test.mch", reg)
m = compiled[0].matcher
block = build_block_tree(BlockSpec(opcode=0x1))
result = await m.try_build(block)
assert result is not None
assert captured[0].a is block
```

- `compile_text` is imported from `indexer.events.mch.compile`. `BlockSpec` and `_registries_with` are imported from `tests.mch.conftest` and `tests.mch.test_compiler_behavioral` respectively (the latter is at module level — copy if needed).

---

## Task 1: Run baseline tests

**Files:** none (verification only)

- [ ] **Step 1: Confirm 89 mch tests pass**

Run:
```
S:/Dev/H-dump/tonscan-indexator/venv0/Scripts/python.exe -m pytest tests/mch/ --tb=no -q
```
from `S:/Dev/ton-indexer-clear/indexer`. Expected output ends with `89 passed`.

- [ ] **Step 2: Note baseline test count**

If the count is not 89, stop and reconcile with the spec before continuing. Some new test may have been added since the spec was written. Record the baseline number — every subsequent task adds tests on top of it.

---

## Task 2: Phase 1 edge case — alternative branches with captures

**Files:**
- Modify: `tests/mch/test_compiler_behavioral.py` (append at end of file)

**Rationale:** Spec §2.2.1. The walker should descend into the firing branch of an `OrMatcher` and bind only its capture; the non-firing branch's capture stays `None`.

- [ ] **Step 1: Add the test**

Append to `tests/mch/test_compiler_behavioral.py`:

```python
@pytest.mark.asyncio
async def test_alternative_branch_capture_firing_branch_binds(build_block_tree):
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3

    matcher alt_branches {
        produces MB
        build    build_alt_branches
        entry    @a

        op A @a -> ( op B @b | op C @c )
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2, "C": 0x3}, "build_alt_branches", "MB")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    # B branch fires
    tree_b = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x2)]))
    assert await m.try_build(tree_b) is not None
    assert len(captured) == 1
    assert captured[0].a is tree_b
    assert captured[0].b is tree_b.next_blocks[0]
    assert captured[0].c is None

    # C branch fires
    captured.clear()
    tree_c = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x3)]))
    assert await m.try_build(tree_c) is not None
    assert len(captured) == 1
    assert captured[0].a is tree_c
    assert captured[0].b is None
    assert captured[0].c is tree_c.next_blocks[0]
```

- [ ] **Step 2: Run the test**

Run:
```
S:/Dev/H-dump/tonscan-indexator/venv0/Scripts/python.exe -m pytest tests/mch/test_compiler_behavioral.py::test_alternative_branch_capture_firing_branch_binds -v
```
Expected: **PASS** (the walker should already handle this correctly).

- [ ] **Step 3: If FAIL — investigate and decide**

If it fails, read the walker (`indexer/events/mch/compiler.py:285-291`) and predict why. The `OrMatcher` branch in `_walk_matcher` picks the first sub whose `test_self` matches. If the failing branch's capture is leaking, the issue is likely that `_walk_matcher` is recursing into the wrong sub-matcher. Apply a surgical fix (<30 LOC) and re-run. If the fix would be larger, mark this edge case as deferred and document in the spec.

---

## Task 3: Phase 1 edge case — where predicate combined with capture

**Files:**
- Modify: `tests/mch/test_compiler_behavioral.py` (append)

**Rationale:** Spec §2.2.2. The `where` clause combines via wrapper lambda in `_lower_node` (compiler.py:503-508). Walker calls `m.test_self`, which is the combined predicate. Capture should bind only when both succeed.

- [ ] **Step 1: Add the test**

```python
@pytest.mark.asyncio
async def test_where_predicate_filters_then_binds_capture(build_block_tree):
    src = """
    opcode A = 0x1
    predicate is_special

    matcher where_pred {
        produces MB
        build    build_where_pred
        entry    @a

        op A @a where is_special
    }
    """
    reg, captured = _registries_with({"A": 0x1}, "build_where_pred", "MB")

    is_special_calls: list = []

    @reg.register_predicate("is_special")
    def is_special(block):
        is_special_calls.append(block)
        return getattr(block, "special_flag", False)

    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    # Block without special flag — must not match
    plain = build_block_tree(BlockSpec(opcode=0x1))
    result = await m.try_build(plain)
    assert not result
    assert len(captured) == 0

    # Block with special flag — matches and binds
    special = build_block_tree(BlockSpec(opcode=0x1))
    special.special_flag = True
    result = await m.try_build(special)
    assert result is not None
    assert len(captured) == 1
    assert captured[0].a is special
```

- [ ] **Step 2: Run the test**

Run:
```
S:/Dev/H-dump/tonscan-indexator/venv0/Scripts/python.exe -m pytest tests/mch/test_compiler_behavioral.py::test_where_predicate_filters_then_binds_capture -v
```
Expected: **PASS**.

- [ ] **Step 3: If FAIL — investigate and decide**

If the predicate isn't being called, check `_lower_node`'s wrapper lambda (compiler.py:503-508). If the predicate is called but binding still fails, the walker is advancing cursor before testing `test_self`. Surgical fix (<30 LOC) or defer.

---

## Task 4: Phase 1 edge case — parent-chain capture

**Files:**
- Modify: `tests/mch/test_compiler_behavioral.py` (append)

**Rationale:** Spec §2.2.3 (flagged Medium risk — never tested). Walker recurses into `parent_matcher` LAST after anchor + child + children. Engine appends parent_matcher blocks to `other_blocks` last (`basic_matchers.py:64-72`). Cursor alignment should hold.

- [ ] **Step 1: Add the test**

```python
@pytest.mark.asyncio
async def test_parent_chain_capture_binding(build_block_tree):
    src = """
    opcode A = 0x1
    opcode B = 0x2

    matcher parent_chain {
        produces MB
        build    build_parent_chain
        entry    @a

        op A @a <- op B @b
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2}, "build_parent_chain", "MB")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    # Tree: B(parent) -> A(child). The matcher fires on A.
    tree = build_block_tree(BlockSpec(opcode=0x2, children=[BlockSpec(opcode=0x1)]))
    a_block = tree.next_blocks[0]
    assert a_block.previous_block is tree

    result = await m.try_build(a_block)
    assert result is not None
    assert len(captured) == 1
    assert captured[0].a is a_block
    assert captured[0].b is tree, "parent chain capture must bind to A's previous_block"
```

- [ ] **Step 2: Run the test**

Run:
```
S:/Dev/H-dump/tonscan-indexator/venv0/Scripts/python.exe -m pytest tests/mch/test_compiler_behavioral.py::test_parent_chain_capture_binding -v
```
Expected: **PASS** if walker is correct. **FAIL** if walker doesn't visit parent_matcher correctly.

- [ ] **Step 3: If FAIL — fix the walker**

The walker's parent_matcher recursion is at `compiler.py:308-309`. Likely issue: by the time we get to parent_matcher, the cursor has been advanced past anchor (and any child blocks), so `all_blocks[cursor]` should be the parent block. If it's not aligned, the engine may be emitting parent blocks in a different order than the walker expects. Trace through what `process_parent_matcher` extends into `blocks` and adjust.

Surgical fix budget: <30 LOC. If exceeded, defer this edge case.

---

## Task 5: Phase 1 edge case — btype anchor capture

**Files:**
- Modify: `tests/mch/test_compiler_behavioral.py` (append)

**Rationale:** Spec §2.2.4. `BlockTypeMatcher` anchor tagged with `_mch_capture_name`. Should work without changes.

- [ ] **Step 1: Add the test**

```python
@pytest.mark.asyncio
async def test_btype_anchor_capture_binding(build_block_tree):
    src = """
    matcher btype_anchor {
        produces MB
        build    build_btype_anchor
        entry    @j

        btype jetton_transfer @j
    }
    """
    reg, captured = _registries_with({}, "build_btype_anchor", "MB")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    tree = build_block_tree(BlockSpec(btype="jetton_transfer"))
    result = await m.try_build(tree)
    assert result is not None
    assert len(captured) == 1
    assert captured[0].j is tree
```

- [ ] **Step 2: Run the test**

Run:
```
S:/Dev/H-dump/tonscan-indexator/venv0/Scripts/python.exe -m pytest tests/mch/test_compiler_behavioral.py::test_btype_anchor_capture_binding -v
```
Expected: **PASS**.

- [ ] **Step 3: If FAIL — investigate**

Most likely cause: the anchor capture is not being tagged via `_mch_capture_name` because the anchor's instance is the synthesized subclass with capture from `anchor_capture`. Check `compiler.py:147-148`. Surgical fix (<30 LOC) or defer.

---

## Task 6: Phase 1 edge case — any head capture

**Files:**
- Modify: `tests/mch/test_compiler_behavioral.py` (append)

**Rationale:** Spec §2.2.5. `GenericMatcher` with `test_self_func=lambda _b: True`. Should match anything.

- [ ] **Step 1: Add the test**

```python
@pytest.mark.asyncio
async def test_any_head_capture_binding(build_block_tree):
    src = """
    opcode A = 0x1

    matcher any_head {
        produces MB
        build    build_any_head
        entry    @a

        op A @a -> any @x
    }
    """
    reg, captured = _registries_with({"A": 0x1}, "build_any_head", "MB")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    tree = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(btype="some_random_btype")]))
    result = await m.try_build(tree)
    assert result is not None
    assert len(captured) == 1
    assert captured[0].a is tree
    assert captured[0].x is tree.next_blocks[0]
```

- [ ] **Step 2: Run the test**

Run:
```
S:/Dev/H-dump/tonscan-indexator/venv0/Scripts/python.exe -m pytest tests/mch/test_compiler_behavioral.py::test_any_head_capture_binding -v
```
Expected: **PASS**.

- [ ] **Step 3: If FAIL — investigate**

Check `_lower_node`'s AnyHead branch (compiler.py:499-500). Surgical fix or defer.

---

## Task 7: Phase 1 edge case — mixed op+btype chain capture

**Files:**
- Modify: `tests/mch/test_compiler_behavioral.py` (append)

**Rationale:** Spec §2.2.6. Mixed chains should bind both kinds of captures.

- [ ] **Step 1: Add the test**

```python
@pytest.mark.asyncio
async def test_mixed_op_and_btype_chain_capture_binding(build_block_tree):
    src = """
    opcode A = 0x1

    matcher mixed_chain {
        produces MB
        build    build_mixed_chain
        entry    @a

        op A @a -> btype jetton_transfer @j
    }
    """
    reg, captured = _registries_with({"A": 0x1}, "build_mixed_chain", "MB")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    tree = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(btype="jetton_transfer")]))
    result = await m.try_build(tree)
    assert result is not None
    assert len(captured) == 1
    assert captured[0].a is tree
    assert captured[0].j is tree.next_blocks[0]
```

- [ ] **Step 2: Run the test**

Run:
```
S:/Dev/H-dump/tonscan-indexator/venv0/Scripts/python.exe -m pytest tests/mch/test_compiler_behavioral.py::test_mixed_op_and_btype_chain_capture_binding -v
```
Expected: **PASS**.

- [ ] **Step 3: If FAIL — investigate**

Walk through `_lower_atom` → `_lower_node` for the BTypeHead in non-anchor position. If binding fails, the issue is in how the chain assembles. Surgical fix or defer.

---

## Task 8: Phase 1 edge case — children block captures across sibling order

**Files:**
- Modify: `tests/mch/test_compiler_behavioral.py` (append)

**Rationale:** Spec §2.2.7 (flagged Medium risk). Engine's `process_children_matchers` (basic_matchers.py:89-118) consumes siblings in `children_matchers` iteration order, not in `next_blocks` order. Walker's recursion into children_matchers in list order should align — but only if `other_blocks` is also in matchers-order. The engine appends `try_build` results in matchers-order, so this should hold.

- [ ] **Step 1: Add the test**

```python
@pytest.mark.asyncio
async def test_children_block_captures_bind_regardless_of_sibling_order(build_block_tree):
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    opcode X = 0x99

    matcher children_block {
        produces MB
        build    build_children_block
        entry    @a

        op A @a -> {
            op B @b
            op C @c
        }
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2, "C": 0x3, "X": 0x99},
                                     "build_children_block", "MB")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    # Order 1: B, C
    captured.clear()
    tree1 = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=0x2), BlockSpec(opcode=0x3),
    ]))
    await m.try_build(tree1)
    assert len(captured) == 1
    assert captured[0].b is tree1.next_blocks[0]
    assert captured[0].c is tree1.next_blocks[1]

    # Order 2: C, B (siblings reversed in tree)
    captured.clear()
    tree2 = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=0x3), BlockSpec(opcode=0x2),
    ]))
    await m.try_build(tree2)
    assert len(captured) == 1, "children block should match regardless of sibling order"
    assert captured[0].b is tree2.next_blocks[1], "b capture must be the B block"
    assert captured[0].c is tree2.next_blocks[0], "c capture must be the C block"

    # Order 3: X (unrelated), B, C — unrelated sibling should not break the match
    captured.clear()
    tree3 = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=0x99), BlockSpec(opcode=0x2), BlockSpec(opcode=0x3),
    ]))
    await m.try_build(tree3)
    assert len(captured) == 1
    assert captured[0].b is tree3.next_blocks[1]
    assert captured[0].c is tree3.next_blocks[2]
```

- [ ] **Step 2: Run the test**

Run:
```
S:/Dev/H-dump/tonscan-indexator/venv0/Scripts/python.exe -m pytest tests/mch/test_compiler_behavioral.py::test_children_block_captures_bind_regardless_of_sibling_order -v
```
Expected: **PASS on all three orderings.**

**Rationale (added after independent review):** `process_children_matchers` (basic_matchers.py:89-118) returns blocks in **matchers-iteration order**, not tree-sibling order. For tree `[C, B]` with matchers `[B-matcher, C-matcher]`, the engine yields `other_blocks = [B_block, C_block]` — the walker's cursor walks the same matchers-iteration order. Tree sibling order never reaches the walker. Verified by tracing through both engine and walker for all three orderings.

- [ ] **Step 3: If FAIL — investigate**

If any ordering fails despite the analysis above, the bug is in either:
- `_walk_matcher`'s OrMatcher branch interaction (e.g., if children_matchers contain OrMatchers whose test_self picks a wrong sub-matcher), or
- A mismatch between `process_children_matchers`'s return order assumption and reality (very unlikely — it's well-tested).

Read both code paths and reconcile. Surgical fix budget: <30 LOC. If exceeded, defer the failing orderings via `pytest.skip` with a spec follow-up.

---

## Task 9: Update jetton-transfer integration test (capture assertions)

**Files:**
- Modify: `tests/mch/integration/test_jetton_transfer.py:39-62`

**Rationale:** Spec §4.2. Remove the stale comment claiming the extractor returns `None` for symbolic-opcode captures; assert on capture content.

- [ ] **Step 1: Replace the builder block**

In `tests/mch/integration/test_jetton_transfer.py`, replace lines 39-62 (the `_make_registries` function) with:

```python
def _make_registries(captured_anchors: list | None = None) -> Registries:
    reg = Registries()
    reg.opcodes.update({
        "JettonTransfer":         JettonTransfer.opcode,
        "JettonInternalTransfer": JettonInternalTransfer.opcode,
        "JettonNotify":           JettonNotify.opcode,
    })
    reg.block_types["JettonTransferBlock"] = JettonTransferBlock

    @reg.register_builder("build_jetton_transfer_dsl")
    async def build_jetton_transfer_dsl(m):
        # Phase 1 capture extraction binds named captures positionally; assert that.
        assert m.transfer is not None, "anchor capture must be populated"
        assert getattr(m.transfer, "opcode", None) == JettonTransfer.opcode, (
            f"m.transfer.opcode = {getattr(m.transfer, 'opcode', None)!r}, "
            f"expected {JettonTransfer.opcode!r}"
        )
        assert m.internal is not None, "internal capture must be populated"
        assert getattr(m.internal, "opcode", None) == JettonInternalTransfer.opcode
        # m.notify is `maybe`-wrapped — may legitimately be None on traces with no notify leg.
        if m.notify is not None:
            assert getattr(m.notify, "opcode", None) == JettonNotify.opcode
        if captured_anchors is not None:
            captured_anchors.append(m.transfer)
        return JettonTransferBlock({}, None)

    return reg
```

The key changes versus the original (lines 48-62):
- Stale comment block (lines 51-60) deleted.
- Builder asserts `m.transfer.opcode`, `m.internal.opcode`, and (when non-None) `m.notify.opcode`.
- Optional `captured_anchors` parameter records the anchor blocks so the test can verify absorption (`block.parent is not None` after match).

- [ ] **Step 2: Update the test to verify absorption explicitly**

After Phase 2's `merge_blocks` call lands, every anchor block consumed by a successful DSL match should have `parent is not None`. Update the test body (around lines 112-139) to capture anchors and assert:

```python
@pytest.mark.asyncio
@pytest.mark.trace_file("KYQfjYD1YybzQIuoJwkoXL-Y-apztmw6AGjIcPhotjQ=.lz4")
async def test_dsl_matches_hand_written_jetton_transfer(trace_data):
    trace, _ = trace_data

    # Compile the DSL with anchor-capture list.
    captured_anchors: list = []
    reg = _make_registries(captured_anchors)
    compiled = compile_text(JETTON_TRANSFER_MCH, "jetton_transfer.mch", reg)
    dsl_matcher = compiled[0].matcher

    dsl_results = await _run_matcher(dsl_matcher, trace)
    hw_matcher = JettonTransferBlockMatcher()
    hw_results = await _run_matcher(hw_matcher, trace)

    dsl_jt = [b for b in dsl_results if isinstance(b, JettonTransferBlock)]
    hw_jt = [b for b in hw_results if isinstance(b, JettonTransferBlock)]

    assert len(hw_jt) > 0, "hand-written matcher found no jetton transfers - bad fixture"
    assert len(dsl_jt) == len(hw_jt), (
        f"DSL matched {len(dsl_jt)} jetton_transfer blocks, "
        f"hand-written matched {len(hw_jt)}"
    )

    # Phase 2 absorption: every consumed anchor block must have parent set.
    assert len(captured_anchors) >= 1
    for anchor in captured_anchors:
        assert anchor.parent is not None, (
            "Phase 2: merge_blocks must wire anchor.parent to the produced block"
        )
```

- [ ] **Step 3: Run the integration test**

Run:
```
S:/Dev/H-dump/tonscan-indexator/venv0/Scripts/python.exe -m pytest tests/mch/integration/test_jetton_transfer.py -v
```
Expected: **PASS** after Task 12 lands (capture assertions and parent assertion both pass). **NOTE:** This task runs the builder asserts but Task 12 hasn't been applied yet — so the parent assertion will fail until Task 12 ships. That's expected; re-run after Task 12 step 3. If the builder's capture assertions fail at this stage, the walker has a real bug.

---

## Task 10: Update stonfi-v2 integration test (capture assertions)

**Files:**
- Modify: `tests/mch/integration/test_stonfi_v2_swap.py:63-87`

**Rationale:** Spec §4.2. Mirror Task 9 for the stonfi-v2 case. `m.swap` and `m.payout` are outside the recursive rule, so they MUST bind. Captures inside `next_hops` are Phase 3 — not asserted.

- [ ] **Step 1: Replace the builder block**

Replace lines 63-87 (the `_make_registries` function) with:

```python
def _make_registries(captured_anchors: list | None = None) -> Registries:
    reg = Registries()
    reg.opcodes.update({
        "StonfiV2Swap":           0x6664de2a,
        "StonfiV2PayTo":          0x657b54f5,
        "JettonNotify":           JettonNotify.opcode,
        "PTonTransfer":           PTonTransfer.opcode,
        "JettonInternalTransfer": JettonInternalTransfer.opcode,
    })
    reg.block_types["JettonSwapBlock"] = JettonSwapBlock

    # Stub pton_self_transfer — for the test we allow any internal transfer.
    # The hand-written matcher's pton-validation logic is too contract-specific
    # to be exercised here; structural parity is the v0 goal.
    @reg.register_predicate("pton_self_transfer")
    def pton_self_transfer(block) -> bool:
        return True

    @reg.register_builder("build_stonfi_v2_swap_dsl")
    async def build_stonfi_v2_swap_dsl(m):
        # Phase 1 capture extraction: m.swap and m.payout are outside the recursive
        # rule and must bind. Captures inside `next_hops` (none today) would be None
        # under Phase 1 — that's Phase 3's job.
        assert m.swap is not None, "anchor capture must be populated"
        assert getattr(m.swap, "opcode", None) == 0x6664de2a
        assert m.payout is not None, "payout capture must be populated"
        assert getattr(m.payout, "opcode", None) == 0x657b54f5
        if captured_anchors is not None:
            captured_anchors.append(m.swap)
        return JettonSwapBlock({})

    return reg
```

Changes:
- Stale comment at lines 83-85 deleted.
- Builder asserts `m.swap.opcode` and `m.payout.opcode`.
- Optional `captured_anchors` parameter to record absorption check inputs.

- [ ] **Step 2: Update the test to verify absorption explicitly**

Update the test body (around lines 142-169):

```python
@pytest.mark.asyncio
@pytest.mark.trace_file("vdNpUDA43aIw6QlbcKMgi7q4S0MnJAVTfXVRqbAt5Tw=.lz4")
async def test_dsl_matches_hand_written_stonfi_v2_swap(trace_data):
    trace, _ = trace_data

    captured_anchors: list = []
    reg = _make_registries(captured_anchors)
    compiled = compile_text(STONFI_V2_SWAP_MCH, "stonfi_v2_swap.mch", reg)
    dsl_matcher = compiled[0].matcher

    dsl_results = await _run_matcher(dsl_matcher, trace)
    hw_results = await _run_matcher(StonfiV2SwapBlockMatcher(), trace)

    dsl_swaps = [b for b in dsl_results if isinstance(b, JettonSwapBlock)]
    hw_swaps = [b for b in hw_results if isinstance(b, JettonSwapBlock)]

    print(f"\n[stonfi-v2-swap] DSL={len(dsl_swaps)} HW={len(hw_swaps)}")
    assert len(hw_swaps) > 0, "hand-written stonfi v2 matcher found no swaps - bad fixture"
    assert len(dsl_swaps) == len(hw_swaps), (
        f"DSL matched {len(dsl_swaps)} swaps, hand-written matched {len(hw_swaps)}"
    )

    # Phase 2 absorption check.
    assert len(captured_anchors) >= 1
    for anchor in captured_anchors:
        assert anchor.parent is not None, (
            "Phase 2: merge_blocks must wire anchor.parent to the produced block"
        )
```

- [ ] **Step 3: Run the integration test**

Run:
```
S:/Dev/H-dump/tonscan-indexator/venv0/Scripts/python.exe -m pytest tests/mch/integration/test_stonfi_v2_swap.py -v
```
Expected: **PASS after Task 12 lands** (parent assertion needs `merge_blocks` to be called by the synthesized `build_block`). Before Task 12, the capture assertions pass but the parent assertion fails — that's expected. Re-run after Task 12 step 3.

---

## Task 11: Phase 2 — write failing test for `build_block` return shape

**Files:**
- Modify: `tests/mch/test_compiler_behavioral.py` (append)

**Rationale:** Spec §3.2 + amendment. After `try_build` on a chain `A -> B -> C`, the returned list should be `[produced, A_block, B_block, C_block]` (with excess/bounce auto-appended after).

- [ ] **Step 1: Add the test (will FAIL initially)**

The synthetic block tree built by `build_block_tree` has empty `event_nodes`. The real `merge_blocks` calls `calculate_min_max_lt`, which crashes on empty event_nodes via `min([])`. So we use a `_MarkerBlock` that overrides `merge_blocks` to record its call (rather than executing the real merge). This decouples return-shape verification from the engine's merge-side-effects, which we exercise on real traces in Tasks 9 and 10.

```python
@pytest.mark.asyncio
async def test_build_block_returns_full_consumed_set(build_block_tree):
    """Phase 2: synthesized build_block must return [produced, block, *other_blocks]
    AND call produced.merge_blocks([block] + other_blocks) for absorption."""
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3

    matcher full_consumed {
        produces MB
        build    build_full_consumed
        entry    @a

        op A @a -> op B @b -> op C @c
    }
    """
    reg = Registries()
    reg.opcodes.update({"A": 0x1, "B": 0x2, "C": 0x3})

    from indexer.events.blocks.core import Block

    class _MarkerBlock(Block):
        def __init__(self):
            super().__init__("marker", [], None)
            self.merge_calls: list = []
        def merge_blocks(self, blocks):  # type: ignore[override]
            # Capture the call without executing the real merge — fake blocks lack
            # event_nodes and would crash calculate_min_max_lt.
            self.merge_calls.append(list(blocks))

    reg.block_types["MB"] = _MarkerBlock

    @reg.register_builder("build_full_consumed")
    async def _build(m):
        return _MarkerBlock()

    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    a = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=0x2, children=[BlockSpec(opcode=0x3)]),
    ]))
    b = a.next_blocks[0]
    c = b.next_blocks[0]

    result = await matcher.try_build(a)
    assert result is not None
    # Return list shape: [produced] only — mirrors hand-written matchers
    # (e.g. elections.py:46-47). Absorption is via merge_blocks side effect.
    # Excess/bounce auto-append doesn't fire here because tree has no excess/bounce blocks.
    assert len(result) == 1, f"build_block must return single-element [produced], got {len(result)} elements: {result!r}"
    assert isinstance(result[0], _MarkerBlock), "first (only) element must be produced block"
    produced = result[0]
    # Absorption: merge_blocks called once with [a, b, c].
    assert len(produced.merge_calls) == 1, f"expected one merge_blocks call, got {len(produced.merge_calls)}"
    assert produced.merge_calls[0] == [a, b, c], (
        f"merge_blocks called with wrong list: {produced.merge_calls[0]!r}"
    )
```

- [ ] **Step 2: Run — confirm FAIL**

Run:
```
S:/Dev/H-dump/tonscan-indexator/venv0/Scripts/python.exe -m pytest tests/mch/test_compiler_behavioral.py::test_build_block_returns_full_consumed_set -v
```
Expected: **FAIL**. Current implementation returns `[produced]` (length 1) but never calls `merge_blocks`, so `len(produced.merge_calls) == 1` fails. This is the bug Phase 2 fixes.

---

## Task 12: Phase 2 — implement the fix in `compiler.py`

**Files:**
- Modify: `indexer/events/mch/compiler.py:189-197` (the `build_block` closure inside `_make_subclass`)

**Rationale:** Spec §3.2 amendment. Call `produced.merge_blocks([block] + other_blocks)` for absorption, return the full consumed list.

- [ ] **Step 1: Edit the closure**

In `indexer/events/mch/compiler.py`, find the `build_block` closure (around lines 189-197 inside `_make_subclass`):

```python
    async def build_block(self, block, other_blocks):  # type: ignore[override]
        kwargs = _extract_captures(self, block, other_blocks, type(self)._mch_capture_names)
        m = type(self)._mch_match_cls(**kwargs)
        builder = registries.builders.get(builder_name)
        if builder is None:
            raise RuntimeError(f"builder {builder_name!r} is not registered at run time")
        produced = await builder(m)
        out_blocks = [produced] if produced is not None else []
        return out_blocks
```

Replace with:

```python
    async def build_block(self, block, other_blocks):  # type: ignore[override]
        kwargs = _extract_captures(self, block, other_blocks, type(self)._mch_capture_names)
        m = type(self)._mch_match_cls(**kwargs)
        builder = registries.builders.get(builder_name)
        if builder is None:
            raise RuntimeError(f"builder {builder_name!r} is not registered at run time")
        produced = await builder(m)
        if produced is None:
            return []
        # Mirror hand-written matchers exactly (e.g. ElectionDepositStakeBlockMatcher
        # at elections.py:46-47): call merge_blocks to wire absorbed blocks'
        # parent linkage, and return [produced] only. The return list advertises the
        # logically-new block; absorption is via the merge_blocks side effect on
        # block.parent. Returning the consumed-blocks list would pollute upstream
        # composers' other_blocks with already-absorbed children.
        produced.merge_blocks([block] + other_blocks)
        return [produced]
```

- [ ] **Step 2: Run the Phase 2 test — confirm PASS**

Run:
```
S:/Dev/H-dump/tonscan-indexator/venv0/Scripts/python.exe -m pytest tests/mch/test_compiler_behavioral.py::test_build_block_returns_full_consumed_set -v
```
Expected: **PASS**.

- [ ] **Step 3: Run all behavioral tests — confirm no regression**

Run:
```
S:/Dev/H-dump/tonscan-indexator/venv0/Scripts/python.exe -m pytest tests/mch/test_compiler_behavioral.py -v
```
Expected: all behavioral tests pass, including Tasks 2-8 + the new Phase 2 test. Any failure here indicates the `merge_blocks` call broke an existing test — investigate immediately (likely a fake Block in a test that doesn't support `merge_blocks`).

- [ ] **Step 4: Run integration tests — confirm capture + absorption checks pass**

Run:
```
S:/Dev/H-dump/tonscan-indexator/venv0/Scripts/python.exe -m pytest tests/mch/integration/ -v
```
Expected: both `test_dsl_matches_hand_written_jetton_transfer` and `test_dsl_matches_hand_written_stonfi_v2_swap` pass — including the new `anchor.parent is not None` assertions added in Tasks 9 and 10.

If integration tests crash with `TypeError: exceptions must derive from BaseException` or similar from `Block.merge_blocks` (`core.py:193` does `raise "Earliest common block not found"` — a bare string raise), the real trace's block topology has a disconnected component that `_ensure_earliest_common_block` can't resolve. That's a latent bug in `merge_blocks` itself, not in our change — fix it by converting the bare string to a real exception class (e.g. `raise RuntimeError("Earliest common block not found")`) as a small drive-by fix, and re-run. If the underlying topology issue is real (no earliest common block exists for this match), the DSL pattern needs adjusting; flag and pause.

---

## Task 13: Phase 2 — test `produced is None` rejection path

**Files:**
- Modify: `tests/mch/test_compiler_behavioral.py` (append)

**Rationale:** Spec §3.3 + §4.1. When the builder returns `None`, `build_block` must return `[]` so the engine does not absorb anything. Important: the engine's auto-append step (basic_matchers.py:53-57) will still try to append excess/bounce blocks to the returned list. If that happens with a rejection, we'd leak absorbed blocks. The test verifies: after rejection, no block in the tree has `parent` set.

- [ ] **Step 1: Add the test — two variants**

Two variants in one test. Variant A: simple tree with no children, verifies the trivial rejection path. Variant B: tree with an excess child block — exercises the latent leak the engine's auto-append step could trigger.

Add to `tests/mch/test_compiler_behavioral.py`:

```python
@pytest.mark.asyncio
async def test_build_block_empty_when_builder_returns_none(build_block_tree):
    """Phase 2: when builder returns None (match rejected), no absorption happens
    and no excess/bounce leak occurs."""
    from indexer.events.blocks.messages import ExcessMessage

    src = """
    opcode A = 0x1

    matcher rejecting {
        produces MB
        build    build_rejecting
        entry    @a

        op A @a
    }
    """
    reg = Registries()
    reg.opcodes.update({"A": 0x1})
    reg.block_types["MB"] = type("MB", (), {})

    @reg.register_builder("build_rejecting")
    async def _build(m):
        return None  # always rejects

    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    # Variant A: trivial tree, no excess children. Must short-circuit cleanly.
    bare = build_block_tree(BlockSpec(opcode=0x1))
    result = await matcher.try_build(bare)
    assert not result, f"rejected match (no excess) must produce empty result, got {result!r}"
    assert bare.parent is None, "rejected match must not set block.parent"

    # Variant B: tree where anchor has an excess child block. The engine's
    # auto-append step (basic_matchers.py:53-57) appends excess blocks to whatever
    # build_block returns. If build_block returns [], auto-append turns it into
    # [excess_block] — a leak. Verify this doesn't happen.
    with_excess = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=ExcessMessage.opcode),
    ]))
    result2 = await matcher.try_build(with_excess)
    assert not result2, (
        f"rejected match must produce empty result even with excess children, got {result2!r}. "
        f"If non-empty, the auto-append leaked excess blocks into a rejected match."
    )
    assert with_excess.parent is None
    assert with_excess.next_blocks[0].parent is None, "excess block must not be absorbed by rejected match"
```

- [ ] **Step 2: Run the test**

Run:
```
S:/Dev/H-dump/tonscan-indexator/venv0/Scripts/python.exe -m pytest tests/mch/test_compiler_behavioral.py::test_build_block_empty_when_builder_returns_none -v
```
Expected outcomes:
- **Variant A PASSES** unconditionally — no excess children in the tree, so auto-append iterates nothing.
- **Variant B likely FAILS** under the current code path. The engine's auto-append at `basic_matchers.py:53-57` mutates `r` in place: when `r = []` and `block.next_blocks` contains an `ExcessMessage.opcode` block, `r.append(...)` turns the result into `[excess_block]`. The `assert not result2` will fail.

- [ ] **Step 3: If Variant B FAILS — fix the rejection path**

Override `try_build` in the synthesized subclass to short-circuit before `build_block` when the builder will reject. Cleanest approach: keep the existing `build_block` returning `[]` on rejection, AND override `try_build` to check the result and return `None` instead of letting the base class auto-append.

Add to `_make_subclass` in `compiler.py` (alongside `build_block`):

```python
    async def try_build(self, block):  # type: ignore[override]
        result = await base.try_build(self, block)
        # If build_block returned [] (builder rejected), the base's auto-append
        # step may have leaked excess/bounce blocks into result. Treat any
        # rejection-shape result (no produced block at index 0) as a non-match.
        if not result:
            return None
        if not isinstance(result[0], type(self)._mch_match_cls.__bases__[0] if False else object):
            # Defensive: produced block is whatever the user-supplied builder returned.
            # We can't easily type-check it; rely on the `produced is None` short-circuit
            # in build_block to set the empty-list signal.
            pass
        return result
```

This is too speculative. Cleaner alternative: change `build_block` itself to detect rejection and filter the post-auto-append. But `build_block` doesn't see the auto-append result. So the real fix has to live in `try_build`.

**Simplest correct fix** — add a sentinel attribute, set in `build_block` before returning `[]`, checked in an overridden `try_build`:

```python
    async def build_block(self, block, other_blocks):  # unchanged structure
        kwargs = _extract_captures(...)
        m = type(self)._mch_match_cls(**kwargs)
        produced = await builder(m)
        if produced is None:
            self._mch_last_rejected = True  # sentinel
            return []
        self._mch_last_rejected = False
        produced.merge_blocks([block] + other_blocks)
        return [produced]

    async def try_build(self, block):
        self._mch_last_rejected = False
        result = await base.try_build(self, block)
        if getattr(self, "_mch_last_rejected", False):
            return None
        return result
```

Where `base` is the parent class (ContractMatcher / BlockTypeMatcher) captured in the closure. ~12 LOC of additions. Budget: <30 LOC. If exceeded, mark Variant B with `pytest.xfail("excess-leak on rejected match — see issue")` and flag for follow-up.

---

## Task 14: Run full mch test suite

**Files:** none (verification)

- [ ] **Step 1: Run all mch tests**

Run:
```
S:/Dev/H-dump/tonscan-indexator/venv0/Scripts/python.exe -m pytest tests/mch/ -v
```
Expected: 89 baseline + 9 new = **98 tests pass**. If any regression, investigate before continuing.

---

## Task 15: Production smoke check

**Files:** none (verification)

**Rationale:** Spec §4.3. DSL matchers are not wired into production (Phase 10), so `tests/test_actions.py` should be identical to baseline. This is a smoke check that nothing leaked into production code paths.

- [ ] **Step 1: Run production action tests**

Run:
```
S:/Dev/H-dump/tonscan-indexator/venv0/Scripts/python.exe -m pytest tests/test_actions.py --tb=no -q
```
Expected: identical pass/fail count to whatever the baseline is on this branch. Do NOT manually fix any failure in this file — it should not be affected by mch changes.

- [ ] **Step 2: Diff against baseline**

If the baseline had N passing tests in `test_actions.py`, this run must also have N passing tests. If it diverges, the mch changes have leaked into production-touched code — back out the last commit/edit and re-investigate.

---

## Task 16: Update roadmap document

**Files:**
- Modify: `docs/superpowers/plans/2026-05-19-mch-future-roadmap.md`

**Rationale:** Spec §7 definition of done #4. Mark Phases 1 and 2 done.

- [ ] **Step 1: Edit Phase 1 section**

In `docs/superpowers/plans/2026-05-19-mch-future-roadmap.md`, find the `## Phase 1 — Position-id capture extraction` header (line 33). Immediately after the `## Phase 1 …` heading, insert:

```
**Status:** ✅ Done (2026-05-21). The positional walker (`_walk_matcher` in `indexer/events/mch/compiler.py`) was already implemented during v0 — it mirrors the engine's traversal order and binds captures by `_mch_capture_name` tag (semantically equivalent to "position id"). R009 was never wired into the resolver; duplicate-opcode captures have always been accepted. Phase 1 closure added behavioral tests for alternative branches, where-predicate, parent chain, btype anchor, any head, mixed op+btype chains, and children-block sibling order. See `docs/superpowers/specs/2026-05-21-mch-phase-1-2-design.md`.
```

- [ ] **Step 2: Edit Phase 2 section**

Find the `## Phase 2 — \`build_block\` output completeness` header (line 57). Immediately after, insert:

```
**Status:** ✅ Done (2026-05-21). The synthesized `build_block` now calls `produced.merge_blocks([block] + other_blocks)` to wire parent linkage (mirroring all hand-written matchers — e.g. `elections.py:46`, `cocoon.py:210`) and returns `[produced, block, *other_blocks]`. The original spec underestimated the work: it called only for the return-shape change. Discovered during plan writing that `merge_blocks` is the absorption mechanism; without it, the DSL silently failed to mark consumed blocks. See `docs/superpowers/specs/2026-05-21-mch-phase-1-2-design.md` §3.2 amendment.
```

- [ ] **Step 3: Edit the "Recommended execution order" section**

Find the block at lines 290-308. Replace the recommended order with a note that Phases 1 and 2 are done. Keep the dependency graph; just add an annotation.

Before:
```
**Critical path** to "fully capable" + production wiring: 1 → 2 → 4 → 5 → 11 → 10.
```

After:
```
**Critical path** to "fully capable" + production wiring: ~~1 → 2~~ (done) → 4 → 5 → 11 → 10.
```

And similarly in the ASCII dependency graph at lines 292-305, prefix Phase 1 and Phase 2 lines with `✅`.

- [ ] **Step 4: Edit the "Definition of done (the whole roadmap)" section**

Find the section header at line 323. Replace the last bullet (#6) with:

```
6. The R009 / C005 / C006 / C001 diagnostics are either resolved (~~R009~~ confirmed never implemented; C006 once Phase 4 lands) or remain as documented unsupported features (C005 for indirect recursion, C001 for unsupported recursion shapes).
```

---

## Task 17: Final verification

**Files:** none (verification)

- [ ] **Step 1: Full mch test suite**

Run:
```
S:/Dev/H-dump/tonscan-indexator/venv0/Scripts/python.exe -m pytest tests/mch/ -v
```
Expected: 98 passing tests, no failures, no warnings beyond the SQLAlchemy/Pydantic baseline.

- [ ] **Step 2: Working-tree summary**

Run:
```
git -C S:/Dev/ton-indexer-clear/indexer status --short tests/mch/ indexer/events/mch/ docs/superpowers/
```
Expected: `M` markers on the touched files; no unrelated edits.

- [ ] **Step 3: Diff inspection**

Run:
```
git -C S:/Dev/ton-indexer-clear/indexer diff --stat tests/mch/ indexer/events/mch/ docs/superpowers/
```
Confirm: `tests/mch/test_compiler_behavioral.py` grew by ~250 lines; `tests/mch/integration/*.py` updated; `indexer/events/mch/compiler.py` changed in ~8 lines; roadmap updated.

- [ ] **Step 4: DO NOT COMMIT**

Per user preference at the top of this plan: leave all changes uncommitted. Report the diff stat to the user; they will commit when ready.

---

## Acceptance summary

When this plan is fully executed, the following are true:

- 98 mch tests pass (89 baseline + 9 new).
- `tests/mch/integration/test_jetton_transfer.py` and `test_stonfi_v2_swap.py` assert on capture content, not just block counts.
- `indexer/events/mch/compiler.py`'s synthesized `build_block` calls `merge_blocks` and returns the full consumed list.
- `docs/superpowers/plans/2026-05-19-mch-future-roadmap.md` marks Phases 1 and 2 as done.
- `tests/test_actions.py` baseline is unchanged.
- No commits made — working tree contains all changes for user review.

---

## Risk-triggered fallbacks (executed only if a step fails as predicted)

| Triggering step | Fallback action |
|---|---|
| Task 4 step 3 FAIL | Fix `_walk_matcher`'s parent_matcher recursion. <30 LOC. If exceeded, defer with skip + spec note. |
| Task 8 step 3 FAIL on orders 2/3 | Replace in-order cursor with per-matcher search loop in walker's `children_matchers` branch. <30 LOC. If exceeded, skip orders 2/3 and document in spec §6. |
| Task 12 step 3 FAIL on existing tests | The `merge_blocks` call needs a real `Block` subclass with `next_blocks`, `previous_block`, `value_flow`, `transient`. If existing tests use fake builders returning plain objects, those builders need updating (similar to Task 11's `_MarkerBlock`). |
| Task 13 step 3 FAIL (excess/bounce leak) | Add a try_build override to the synthesized subclass that short-circuits when builder returns None. ~10 LOC. |
