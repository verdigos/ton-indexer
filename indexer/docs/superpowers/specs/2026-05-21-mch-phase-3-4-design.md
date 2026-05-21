# `.mch` DSL — Phase 3 + Phase 4 design

**Status:** Design (v2, 2026-05-21 — incorporates parallel review feedback).
**Scope:** Implement Phase 3 (list bindings under recursion) and Phase 4 (multi-hop parent chains) of the mch roadmap. Single coherent change set.
**Related:** `docs/superpowers/plans/2026-05-19-mch-future-roadmap.md` (parent roadmap), `docs/superpowers/specs/2026-05-21-mch-phase-1-2-design.md` (predecessor — positional capture walker + `merge_blocks` absorption are the foundation Phase 3 builds on).

**v2 changelog:** Reviewer feedback resolved:
- Phase 4 anchor-in-tail direction bug fixed with explicit pseudocode (§2.3).
- Phase 4 cursor alignment for anchor-in-tail walked through (§2.4).
- Phase 3 termination strategy reworked: `maybe $self` suffix stripped from step branches at lowering time, instead of relying on permissive sentinel — fixes the infinite-loop blocker (§3.3).
- Walker uses `list_binding` (the real attribute on CaptureInfo), not `is_list` (§3.4).
- Synthetic 3-hop tests added; existing stonfi trace not depended on for multi-hop validation (§3.5, §5).
- Cursor advance after recursion explicitly set to `cursor = len(all_blocks)`; contradiction removed (§3.4).
- Mixed-edge / `<-` in alt branch / `<-` in maybe edge cases tested (§2.5).

---

## 1. Scope & goals

### What's in

1. **Phase 4 — Multi-hop parent chains.** Recursive lowering of consecutive `<-` edges into a `parent_matcher.parent_matcher.…` chain. Two sub-cases: anchor-is-head and anchor-in-tail. Removal of `C006_MULTI_HOP_PARENT_UNSUPPORTED` resolver/compiler check.
2. **Phase 3 — List bindings under recursion.** Per-iteration capture tracking inside `RecursiveMatcher`-lowered rules. Captures whose `CaptureInfo.list_binding is True` bind as `list[T]` in trace order (parallel-aligned with peers). Codegen already emits `list[T]`; runtime extractor must populate it. Also fixes a latent v0 termination bug where recursion always ran exactly one iteration.
3. **Roadmap update.** Mark Phases 3 and 4 done in the future-roadmap doc with implementation notes.

### What's out

- Phase 5 `ChildrenBlock` semantic rewrite (separate phase).
- Phases 6–11 (selectivity, ExclusiveOr heuristic, multi-file, codegen QoL, production wiring, corpus parity).
- Stitch/cross-trace mechanics. C++ host.
- C005 indirect-recursion: stays rejected.
- Non-trailing `$self` references inside recursive rule bodies (i.e. self-ref anywhere other than the tail position of a step branch). Pre-existing DSL has no use for this; we document it as unsupported and keep current behavior.
- Captures appearing AFTER a recursion in the same sequence (e.g. `op A @a -> $rule -> op B @b`). Out-of-scope, see §6.

### Success criteria

- All new behavioral tests pass.
- `op A @a <- op B @b <- op C @c` (anchor-head 2-hop) and `... <- op D @d` (3-hop) compile and bind correctly.
- `op X -> op A @a <- op B @b <- op C @c` (anchor-in-tail with parent chain) compiles and binds correctly.
- A synthetic 3-iter recursion test asserts `len(m.step) == 3`, `len(m.a) == 3`, `len(m.b) == 3` with `m.a[i] / m.b[i]` parallel-aligned per iteration.
- A behavioral test demonstrates recursion now actually iterates (regression test against the latent v0 "always-one-iteration" bug).
- Stonfi v2 integration test still passes (outside-recursion captures `m.swap` and `m.payout` remain scalar; structural DSL-vs-HW count unchanged on the existing single-hop trace).
- Baseline mch suite (98 tests after Phase 1/2) plus ~14 new tests, all green. `tests/test_actions.py` baseline unchanged.

---

## 2. Phase 4 — Multi-hop parent chains

### 2.1 Current state

`indexer/events/mch/compiler.py:430-447` (`_lower_parent_chain_from`):
- **Anchor-is-head**: if `parent.tail[0][0] is Edge.PARENT`, return `lower(parent.tail[0][1])`. Single hop only.
- **Anchor-in-tail with edge=PARENT**: when anchor at tail index `i` has incoming edge=PARENT, return `lower(previous)` where previous is `parent.head if i == 0 else parent.tail[i-1][1]`. Single hop only.

`indexer/events/mch/compiler.py:450-480` (`_check_no_multi_hop_parent`):
- Walks AST; on two consecutive PARENT edges in a Sequence's tail, raises `C006_MULTI_HOP_PARENT_UNSUPPORTED`.

Test `tests/mch/test_compiler_structural.py:236-258` (`test_multi_hop_parent_chain_rejected`) expects this rejection. Converted to a positive assertion.

### 2.2 DSL edge semantics (reference)

For a Sequence `A op_atoms…` with `op` ∈ {`->`, `<-`}:
- `A -> B`: AST `Sequence(head=A, tail=[(CHILD, B)])`. B is in A.next_blocks (A is parent of B in block tree).
- `A <- B`: AST `Sequence(head=A, tail=[(PARENT, B)])`. A.previous_block is B (B is parent of A in block tree).

Verified via `tests/mch/test_compiler_behavioral.py::test_parent_chain_capture_binding`: matcher `op A @a <- op B @b` against tree `B(child=A)` binds `m.a=A_block, m.b=A.previous_block=B_block`.

### 2.3 Lowering semantics — multi-hop

Two sub-cases.

#### Sub-case A: anchor is the sequence head

Pattern: `anchor <- op B <- op C <- op D`. AST `Sequence(head=anchor, tail=[(PARENT, B), (PARENT, C), (PARENT, D)])`.

Reading: anchor.previous_block = B, B.previous_block = C, C.previous_block = D. Chain.

Pseudocode:

```python
def _lower_parent_chain_from(parent, pattern, anchor, ctx):
    if not isinstance(parent, ast_.Sequence):
        return None

    # Identify the index in tail from which to start collecting consecutive PARENT edges.
    if parent.head is anchor:
        start_idx = 0
    else:
        # Find anchor in tail.
        anchor_idx = next(
            (i for i, (_e, a) in enumerate(parent.tail) if a is anchor),
            None,
        )
        if anchor_idx is None:
            return None
        # Two further sub-cases inside anchor-in-tail. Handle in caller below.
        start_idx = _resolve_anchor_in_tail_start(parent, anchor, anchor_idx)
        if start_idx is None:
            return None

    # Collect consecutive PARENT-edge atoms from start_idx forward.
    parent_atoms: list[ast_.PatternExpr] = []
    for edge, atom in parent.tail[start_idx:]:
        if edge is not ast_.Edge.PARENT:
            break
        parent_atoms.append(atom)

    if not parent_atoms:
        return None

    # Lower each atom and chain via parent_matcher links.
    first = _lower_atom(parent_atoms[0], ctx)
    current = first
    for atom in parent_atoms[1:]:
        nxt = _lower_atom(atom, ctx)
        _attach_as_parent(current, nxt)
        current = nxt
    return first
```

#### Sub-case B: anchor is in tail

Two further variants:

**B.1.** Anchor has edge=PARENT (existing single-hop case): `... <- anchor`. The edge BEFORE the anchor IS the `<-`. In the existing implementation, this returns `lower(previous)` as anchor.parent_matcher. For multi-hop extension `... <- anchor <- B <- C`: walk forward consecutive PARENT edges from `anchor_idx + 1`.

`_resolve_anchor_in_tail_start(parent, anchor, anchor_idx)` returns `anchor_idx + 1` — BUT we must ALSO emit the previous atom (the "incoming `<-`" semantically pointing at anchor) as the first link. Concretely the result for `op X <- op A @a <- op B`:
- AST `Sequence(head=X, tail=[(PARENT, A), (PARENT, B)])`. anchor=A at i=0 with edge=PARENT.
- A's parent in block-tree per current semantics: take `previous = parent.head = X`. So A.parent_matcher chain starts with `lower(X)`, then chains lower(B) onto X.parent_matcher.

Pseudocode for this variant:

```python
def _resolve_anchor_in_tail_start(parent, anchor, anchor_idx):
    edge_at_anchor, _ = parent.tail[anchor_idx]
    if edge_at_anchor is ast_.Edge.PARENT:
        # Variant B.1: include the "previous" atom as the first parent link, then
        # continue forward through consecutive PARENT edges.
        previous_atom = parent.head if anchor_idx == 0 else parent.tail[anchor_idx - 1][1]
        # Signal to caller that an extra prepended atom is needed.
        return ("prepend", previous_atom, anchor_idx + 1)
    # Variant B.2: anchor's incoming edge is CHILD; only forward parent edges count.
    return anchor_idx + 1
```

(For brevity, the actual implementation flattens this — see §2.7 pseudocode below.)

**B.2.** Anchor has edge=CHILD: `op X -> op A @a <- op B`. AST `Sequence(head=X, tail=[(CHILD, A), (PARENT, B)])`. anchor=A at i=0 with edge=CHILD. Walking forward from anchor_idx+1: tail[1]=(PARENT, B). B is A's parent. Chain: A.parent_matcher = lower(B). Multi-hop extension `... -> op A @a <- op B <- op C`: chain B.parent_matcher = lower(C).

#### Unified pseudocode (final form)

```python
def _lower_parent_chain_from(parent, pattern, anchor, ctx):
    if not isinstance(parent, ast_.Sequence):
        return None

    prepend_previous = False
    start_idx = 0

    if parent.head is anchor:
        if not parent.tail or parent.tail[0][0] is not ast_.Edge.PARENT:
            return None
        start_idx = 0
    else:
        anchor_idx = next(
            (i for i, (_e, a) in enumerate(parent.tail) if a is anchor),
            None,
        )
        if anchor_idx is None:
            return None
        edge_at_anchor, _ = parent.tail[anchor_idx]
        if edge_at_anchor is ast_.Edge.PARENT:
            # B.1: prepend the "previous" atom (head or tail[anchor_idx-1])
            prepend_previous = True
            start_idx = anchor_idx + 1
        else:
            # B.2: skip the incoming-CHILD edge; walk forward PARENT edges.
            start_idx = anchor_idx + 1

    parent_atoms: list[ast_.PatternExpr] = []
    if prepend_previous:
        anchor_idx = next(i for i, (_e, a) in enumerate(parent.tail) if a is anchor)
        previous_atom = parent.head if anchor_idx == 0 else parent.tail[anchor_idx - 1][1]
        parent_atoms.append(previous_atom)
    for edge, atom in parent.tail[start_idx:]:
        if edge is not ast_.Edge.PARENT:
            break
        parent_atoms.append(atom)

    if not parent_atoms:
        return None

    first = _lower_atom(parent_atoms[0], ctx)
    current = first
    for atom in parent_atoms[1:]:
        nxt = _lower_atom(atom, ctx)
        _attach_as_parent(current, nxt)
        current = nxt
    return first


def _attach_as_parent(host, parent):
    if host.parent_matcher is None:
        host.parent_matcher = parent
    else:
        _attach_as_parent(host.parent_matcher, parent)
```

Delete `_check_no_multi_hop_parent` and its call site (`_compile_matcher:90`). Delete the `C006_MULTI_HOP_PARENT_UNSUPPORTED` diagnostic constant.

### 2.4 Walker cursor alignment

The walker `_walk_matcher` (compiler.py:299-340) recurses into `parent_matcher` after `anchor → child → children`. Engine `BlockMatcher.try_build`:

```
blocks = []
child_matched = await process_child_matcher(block, blocks, ...)    # child + children
parent_matched = await process_parent_matcher(block, blocks, ...)  # parent_matcher
build_block(block, blocks)
```

`process_parent_matcher` (basic_matchers.py:64-72):
```
if parent_matcher is not None:
    matcher_parent_blocks = await parent_matcher.try_build(block.previous_block)
    if matcher_parent_blocks is not None:
        blocks.extend(matcher_parent_blocks)
```

So `parent_matcher.try_build(block.previous_block)` is called on the anchor's previous_block. That `try_build` itself recurses: `parent_matcher.parent_matcher.try_build(block.previous_block.previous_block)`, etc. The returned list is the chain in order [parent_block, grandparent_block, …].

Walker's traversal:
```
visit anchor → cursor += 1
if child_matcher: walk(child_matcher, …)      → cursor advances by len(child contrib)
if children_matchers: walk each            → cursor advances by len(children contrib)
if parent_matcher: walk(parent_matcher, …)   → cursor advances by len(parent chain)
```

For multi-hop parent chain at anchor: `_walk_matcher(parent_matcher, …)` recurses normally — visit parent atom, set capture, then recurse into ITS parent_matcher (the next hop). The cursor advance lines up with the order the engine appends to `blocks` (parent → grandparent → great-grandparent).

For anchor-in-tail with both child and parent chains (`op X -> op A @a <- op B`): walker on outer matcher visits A, advances to cursor=1, then visits child_matcher (lower(X)). Wait — `op X -> op A`: X is A's child_matcher? Per existing `_lower_child_chain_from` for anchor-in-tail (compiler.py:411-413), `downstream_atoms` collects CHILD-edge atoms in `parent.tail[anchor_idx + 1:]`. For our case, anchor_idx=0 (anchor=A), tail[1:]=[(PARENT, B)] — no CHILD edges. So child_matcher=None. The X-as-A's-previous-block is NOT lowered as child_matcher (matches existing v0 behavior — `op X -> op A @a` with anchor=A produces no child_matcher because nothing follows the anchor on the CHILD side).

So for `op X -> op A @a <- op B`: walker visits A, cursor=1, no child, no children, then parent_matcher=lower(B). Engine produces blocks=[parent_B]. Walker visits B at cursor=1, binds m.b. Cursor=2. Done. Correct.

For `op X -> op A @a <- op B @b <- op C @c`: same plus B's parent_matcher=lower(C). Engine: A.previous_block=B; B.try_build calls process_parent_matcher with C.try_build(B.previous_block=C). C.try_build returns [C]. B.try_build returns [B, C]. A.try_build returns [A, B, C]. Walker: A → cursor=1, no child, no children, parent=lower(B); walk(lower(B)) → test_self(B)→True, cap m.b, cursor=2, recurse into lower(B).parent_matcher=lower(C); walk(lower(C)) → test_self(C)→True, cap m.c, cursor=3. Done.

Verified: walker handles multi-hop parent chain naturally via existing parent_matcher recursion. No walker change needed for Phase 4.

### 2.5 Tests

In `tests/mch/test_compiler_behavioral.py`:

| # | Test | Asserts |
|---|---|---|
| 4.a | `test_two_hop_parent_chain_anchor_head` | `op A @a <- op B @b <- op C @c` against tree `C(child=B(child=A))`: m.a, m.b, m.c bind. |
| 4.b | `test_three_hop_parent_chain_anchor_head` | `op A @a <- op B @b <- op C @c <- op D @d` against tree `D(child=C(child=B(child=A)))`: all four bind. |
| 4.c | `test_two_hop_parent_chain_partial_match_rejected` | 2-hop chain where root parent has wrong opcode: try_build returns None. |
| 4.d | `test_parent_chain_in_tail_position_child_edge_anchor` | `op X -> op A @a <- op B @b <- op C @c` against tree `X(child=A.then=B(parent_chain).then=C)`: anchor=A, all four bind. (B.2 sub-case.) |
| 4.e | `test_parent_chain_in_tail_position_parent_edge_anchor` | `op X <- op A @a <- op B @b` against tree `A(child=X) -> B(parent=A)` — wait, AST semantics: head=X, A is tail[0] with edge=PARENT (per §2.2: `X <- A` means X.previous_block=A). So X is A's child in block tree. Anchor=A. Tree: A(parent), X(child of A). With Phase 4 B.1: A.parent_matcher chain = [lower(X), then lower(B)]. Wait — that's wrong direction. Variant B.1 prepending `previous_atom = X` as A's first parent is semantically incorrect; it would only validate if A.previous_block is X, but A is X's parent in block tree (X.previous_block=A). Re-examine in §2.6 deferral. |
| 4.f | `test_parent_chain_mixed_with_child_chain` | `op A @a -> op B <- op C @c -> op D` — mixed edge case. Per current v0 lowering: anchor=A, child_chain collects CHILD-edge atoms only → [B, D] (skipping C because edge=PARENT before C). The `<- op C` is dropped by current code. Phase 4 does not introduce new behavior here; test asserts current behavior is preserved (pre-existing limitation documented in §6). |
| 4.g | `test_parent_chain_inside_alternative_branch` | `( op A @a <- op B @b | op X @x )` — one branch has a parent chain. Compiles cleanly. The OrMatcher tries branches; matched branch's captures bind. |
| 4.h | `test_parent_chain_inside_maybe_subtree` | `op A @a <- maybe op B @b <- op C @c` — wait, can `maybe` wrap a parent-chain atom? Per existing parser, `maybe` wraps a single atom. So `maybe op B` is the second atom of the chain. The maybe atom's `optional=True` propagates to its lowered ContractMatcher. Engine's `process_parent_matcher` handles optional via `parent_matched = self.parent_matcher.optional or matcher_parent_blocks is not None`. Phase 4 chain lowering must preserve this. Test confirms. |

Convert `tests/mch/test_compiler_structural.py::test_multi_hop_parent_chain_rejected` to `test_multi_hop_parent_chain_lowers_to_chain` — assert `instance.parent_matcher.parent_matcher` is set and the leaf's opcode matches the chain tail (no exception raised).

### 2.6 Deferred sub-case (Variant B.1)

Variant B.1 — anchor-in-tail with anchor's INCOMING edge being PARENT — is semantically unclear. The existing single-hop code at `compiler.py:443-446` returns the `previous` atom (head or `tail[i-1][1]`) as anchor's parent_matcher. This appears to interpret `X <- A` as "A.previous_block = X". But per §2.2's verified semantics from Phase 1/2 test, `A <- B` (anchor=A as head) means A.previous_block = B. The asymmetric "anchor-in-tail-with-PARENT-edge" case may carry a different historical interpretation.

**Decision:** Phase 4 preserves the existing single-hop B.1 behavior exactly. Multi-hop extension in B.1 walks forward consecutive PARENT edges from `anchor_idx + 1` AND prepends the `previous` atom — as in the pseudocode in §2.3. This is consistent with the existing single-hop case. If real production matchers don't use the B.1 shape (likely — searching `indexer/events/blocks/*.py` finds no matcher with anchor-in-the-middle-via-PARENT pattern), the test `4.e` is permitted to assert whatever the implementation produces, rather than a hand-derived ground truth.

If reviewer/user disagrees with this decision, the alternative is to reject B.1 entirely with a new diagnostic (`C008_AMBIGUOUS_PARENT_EDGE`) and remove the existing single-hop B.1 code. Out of scope unless flagged.

### 2.7 Complexity

Compiler change ~70 LOC (rewrite `_lower_parent_chain_from`, add `_attach_as_parent`, delete `_check_no_multi_hop_parent`). Tests ~250 LOC across 8 cases + 1 structural conversion.

---

## 3. Phase 3 — List bindings under recursion

### 3.1 Current state

`_walk_matcher` (compiler.py:299-340) treats `RecursiveMatcher` as opaque: sets `cursor = len(all_blocks)`. Captures inside the recursive rule body stay `None`.

Codegen DOES emit `list[T]` for captures where `CaptureInfo.list_binding is True`:
- `resolver.py:274-280` sets `list_binding=under_recursive` when visiting captures.
- `codegen.py:87-88` emits `list[T]` when `info.list_binding` is True.
- The runtime Match dataclass currently initializes ALL fields to `None` via the synthesized `build_block` (compiler.py:294 `captures: dict[str, Any] = {name: None for name in capture_names}`). Phase 3 changes this initialization to `[]` for list_binding captures.

### 3.2 Engine recursion mechanics (current)

`RecursiveMatcher.process_child_matcher` (`basic_matchers.py:205-236`):

```
current_blocks = [block]
blocks_to_add  = []
while True:
    block_for_check = current_blocks[:]
    current_blocks  = []
    for c in block_for_check:
        if exit_matcher:
            r = await exit_matcher.try_build(c)
            if r is not None:
                blocks_to_add.extend(r); finished = True
        if finished: break
        r = await repeating_matcher.try_build(c)
        if r is not None:
            blocks_to_add.extend(r)
            for b in r: current_blocks.extend(b.next_blocks)
            break
    if finished: break
    if len(current_blocks) == 0: child_matched = False; break

if len(blocks_to_add) > 0:
    blocks.extend(set(blocks_to_add))    # set() — order lost
    child_matched = True
```

**Latent bug confirmed (reviewer):** when `_lower_recursive_rule` finds no pure-exit branches (the stonfi_v2 case), v0 creates `exit_m = GenericMatcher(test_self_func=lambda _b: True)` (compiler.py:605). On iteration 1, `exit_matcher.try_build(initial_block)` returns `[initial_block]` (GenericMatcher with no child/parent + base build_block → [block]), `finished = True`, loop ends. The recursion runs exactly once regardless of trace depth. The stonfi integration test passes today only because the trace fixture is single-hop.

Phase 3 must address both: (a) per-iteration capture tracking (the headline goal), AND (b) the "always-true exit short-circuits recursion" bug — without (b), the recursion never iterates more than once and list captures would always be 1-element.

### 3.3 Termination strategy — fix the latent bug

**Reviewer-driven revision.** The original draft proposed `exit_matcher=None` + relying on `repeating_matcher.try_build` failing naturally. Reviewer found this would infinite-loop because the `maybe $self` step branch lowers to a permissive `GenericMatcher(always_true)` sentinel (compiler.py:507-508), which always matches → loop never terminates.

**New strategy:** strip `maybe $self` (and equivalents) from step branches at lowering time. After stripping:
- A step branch that consisted ONLY of `maybe $self` is discarded entirely (it carries no actual pattern).
- A step branch like `op A -> op B -> maybe $self` becomes `op A -> op B` (drops the trailing self-ref).
- The repeating_matcher consists ONLY of real concrete-step patterns.
- The engine's iteration loop runs while real patterns match; when no branch matches, `repeating_matcher.try_build` returns None → `current_blocks` may stay empty → loop terminates cleanly via `child_matched = False; break`.

Crucially: when zero iterations succeed (initial block doesn't match step), `blocks_to_add` is empty → `child_matched = False`. The OUTER `_MchRecursiveMatcher` has `optional=True`, so the outer matcher tolerates this (via the standard `optional or len(blocks) > 0` check in `process_child_matcher`).

Pseudocode for the strip:

```python
def _strip_trailing_self_ref(branch, rule_name):
    """Return the branch with a trailing `maybe $rule_name` removed.

    Returns None if the branch is just `maybe $rule_name` alone.
    """
    if isinstance(branch, ast_.Maybe) and isinstance(branch.inner, ast_.RuleRef) and branch.inner.name == rule_name:
        return None
    if isinstance(branch, ast_.Sequence):
        # Check the last tail entry.
        if branch.tail:
            last_edge, last_atom = branch.tail[-1]
            if (
                isinstance(last_atom, ast_.Maybe)
                and isinstance(last_atom.inner, ast_.RuleRef)
                and last_atom.inner.name == rule_name
            ):
                # Strip the last entry. If tail becomes empty, return head alone.
                new_tail = branch.tail[:-1]
                if not new_tail:
                    return branch.head
                return ast_.Sequence(head=branch.head, tail=new_tail, span=branch.span)
    return branch
```

Apply in `_lower_recursive_rule` BEFORE classifying step/exit branches.

For step branches with no trailing self-ref (the pure-step branch contains $self in an unexpected position): document as out-of-scope per §1 — current behavior preserved, the permissive sentinel still fires, and behavior is undefined but rule-shape unusual.

### 3.4 `_MchRecursiveMatcher` subclass

New subclass in `compiler.py`:

```python
class _MchRecursiveMatcher(RecursiveMatcher):
    def __init__(self, repeating_matcher, exit_matcher, optional, has_pure_exit):
        super().__init__(repeating_matcher, exit_matcher, optional)
        self._mch_has_pure_exit = has_pure_exit
        self._mch_iter_blocks: list[list[Block]] = []

    async def process_child_matcher(self, block, blocks, child_matched):
        self._mch_iter_blocks = []
        current_blocks = [block]
        blocks_to_add = []
        finished = False
        while True:
            block_for_check = current_blocks[:]
            current_blocks = []
            iter_consumed: list[Block] = []
            for c in block_for_check:
                if self._mch_has_pure_exit and self.exit_matcher is not None:
                    r = await self.exit_matcher.try_build(c)
                    if r is not None:
                        blocks_to_add.extend(r)
                        finished = True
                if finished:
                    break
                r = await self.repeating_matcher.try_build(c)
                if r is not None:
                    iter_consumed.extend(r)
                    blocks_to_add.extend(r)
                    for b in r:
                        current_blocks.extend(b.next_blocks)
                    break
            if iter_consumed:
                self._mch_iter_blocks.append(iter_consumed)
            if finished:
                break
            if len(current_blocks) == 0:
                child_matched = False
                break

        if len(blocks_to_add) > 0:
            blocks.extend(set(blocks_to_add))  # set() preserved for engine parity
            child_matched = True
        return child_matched
```

Constructor signature change in `_lower_recursive_rule`:
- Compute `has_pure_exit = bool(exit_branches)`.
- If `has_pure_exit`: pass `exit_matcher=<lowered exit>`.
- If not: pass `exit_matcher=None`. (The old `GenericMatcher(always_true)` placeholder is removed.)
- Always pass `optional=True` (preserves v0 behavior).

### 3.5 Walker integration

`_extract_captures` and `_walk_matcher` are updated. The signatures change minimally:

```python
def _extract_captures(root, anchor_block, other_blocks, capture_names, capture_infos):
    """capture_infos: dict[str, CaptureInfo] from ResolvedMatcher.captures."""
    all_blocks = [anchor_block] + list(other_blocks)
    cursor = [0]
    # Initialize list_binding captures to [], scalar captures to None.
    captures: dict[str, Any] = {
        name: ([] if capture_infos[name].list_binding else None)
        for name in capture_names
    }
    _walk_matcher(root, all_blocks, cursor, captures, capture_infos)
    return captures


def _walk_matcher(m, all_blocks, cursor, captures, capture_infos):
    if cursor[0] >= len(all_blocks):
        return

    if isinstance(m, _MchRecursiveMatcher):
        # Per-iteration extraction via the matcher's recorded iteration blocks.
        for iter_blocks in m._mch_iter_blocks:
            if not iter_blocks:
                continue
            iter_cursor = [0]
            iter_captures: dict[str, Any] = {
                name: None for name in captures.keys()
            }
            _walk_matcher(m.repeating_matcher, list(iter_blocks), iter_cursor,
                          iter_captures, capture_infos)
            # Merge: for list-binding captures, append per-iter binding (even None).
            for name, info in capture_infos.items():
                if info.list_binding:
                    captures[name].append(iter_captures.get(name))
        # After processing recursion, advance outer cursor to end-of-buffer.
        # (set() in engine destroys positional alignment for outside-recursion captures
        # after the recursion; this is documented as out-of-scope in §6.)
        cursor[0] = len(all_blocks)
        return

    if isinstance(m, RecursiveMatcher):
        # Non-_Mch RecursiveMatcher (shouldn't occur post-Phase 3, but defensive).
        cursor[0] = len(all_blocks)
        return

    # Rest of walker logic unchanged from Phase 1/2.
    if isinstance(m, OrMatcher):
        block = all_blocks[cursor[0]]
        for sub in m.matchers:
            if sub.test_self(block):
                _walk_matcher(sub, all_blocks, cursor, captures, capture_infos)
                return
        return

    block = all_blocks[cursor[0]]
    if not m.test_self(block):
        return
    cap = getattr(m, "_mch_capture_name", None)
    if cap is not None and cap in captures:
        # Note: for list_binding captures, this code-path should not be reached
        # at top level — those are populated via the recursion loop above.
        # If reached, it's an outside-recursion scalar capture.
        if not (capture_infos.get(cap) and capture_infos[cap].list_binding):
            captures[cap] = block
    cursor[0] += 1
    if m.child_matcher is not None:
        _walk_matcher(m.child_matcher, all_blocks, cursor, captures, capture_infos)
    if m.children_matchers:
        for cm in m.children_matchers:
            _walk_matcher(cm, all_blocks, cursor, captures, capture_infos)
    if m.parent_matcher is not None:
        _walk_matcher(m.parent_matcher, all_blocks, cursor, captures, capture_infos)
```

Cursor advance after recursion: **explicitly `cursor[0] = len(all_blocks)`**. Earlier draft contradicted itself; resolved here. Outside-recursion captures AFTER the recursion are out-of-scope (§6).

### 3.6 Compile-time check (new diagnostic)

When the walker would silently mis-bind, surface a compile-time diagnostic instead. New diagnostic `C007_CAPTURE_AFTER_RECURSION`: emitted when a Sequence contains a `$rule_name` (where `rule_name` is direct-recursive) NOT in tail position, AND any later sibling carries a capture. Walker contract: list captures via `_mch_iter_blocks`; everything after the recursion is `cursor = len(all_blocks)` → unreachable.

Pseudocode (in resolver or compiler pre-check):

```python
def _check_no_capture_after_recursion(pattern, ctx):
    def walk(p):
        if isinstance(p, ast_.Sequence):
            recursion_seen = False
            for edge, atom in [(None, p.head)] + list(p.tail):
                if recursion_seen:
                    if _contains_capture(atom):
                        ctx.bag.error(
                            "C007_CAPTURE_AFTER_RECURSION",
                            "captures placed after a recursive-rule reference in the "
                            "same sequence are not bound; restructure the pattern",
                            atom.span,
                        )
                if _is_direct_recursive_ref(atom):
                    recursion_seen = True
                walk(atom)
        elif isinstance(p, (ast_.Maybe, ast_.Alternative, ast_.ChildrenBlock)):
            for sub in _children(p):
                walk(sub)
    walk(pattern)
```

Add the call to `_compile_matcher`. ~30 LOC.

### 3.7 Tests

In `tests/mch/test_compiler_behavioral.py`:

| # | Test | Asserts |
|---|---|---|
| 3.a | `test_recursive_rule_three_iterations_list_capture` | `rule step = op A @a -> maybe $step`. Trace `A1 → A2 → A3 → (no more)`. `m.a == [A1, A2, A3]`. |
| 3.b | `test_recursive_rule_parallel_captures_aligned` | `rule step = op A @a -> op B @b -> maybe $step`. Trace 3 hops. `m.a=[A1,A2,A3]`, `m.b=[B1,B2,B3]` parallel-aligned. |
| 3.c | `test_recursive_rule_zero_iterations_empty_list` | Recursion where initial block doesn't match step → `m.a == []`, not None. (Verifies outer matcher tolerates via optional=True.) |
| 3.d | `test_recursive_rule_one_iteration_single_element_list` | Single-hop trace: `m.a == [A1]`. |
| 3.e | `test_recursive_rule_termination_no_pure_exit` | Step has only "always recurses" branches (no pure-exit). Verifies recursion now actually iterates > 1 time (regression for §3.2 bug). |
| 3.f | `test_recursive_rule_with_maybe_inside_step` | `rule step = op A @a -> maybe op B @b -> maybe $step`. Trace `A1 → (no B) → A2 → B2 → A3`. `m.a=[A1, A2, A3]`, `m.b=[None, B2, None]`. Per-iter optional capture. |
| 3.g | `test_recursive_rule_outside_captures_remain_scalar` | Pattern with outside-recursion captures (anchor + chain-prefix) and inside-recursion captures. Outside stays scalar. Regression for stonfi-v2-style outer captures. |
| 3.h | `test_capture_after_recursion_rejected` | `op A @a -> $rule -> op B @b` where `$rule` is recursive — compile error `C007_CAPTURE_AFTER_RECURSION`. |
| 3.i | `test_recursive_rule_or_branches_different_captures` | `rule step = ( op A @a | op B @b ) -> maybe $step`. Trace alternates A and B. `m.a` lists A blocks at their iter positions with None at B-iter positions; `m.b` symmetric. (Parallel-list alignment with OrMatcher inside step.) |

Integration test `tests/mch/integration/test_stonfi_v2_swap.py`:
- Builder unchanged for outside captures (`m.swap`, `m.payout` stay scalar).
- Add assertion that `len(captured_anchors) >= 1` and the structural count parity with hand-written matcher is preserved. Since the existing trace is single-hop, the count remains 1; no behavioral regression. DO NOT depend on this trace for multi-hop list assertions — those are in synthetic tests above.

### 3.8 Complexity

Compiler: ~210 LOC across `_strip_trailing_self_ref` (~30), `_MchRecursiveMatcher` (~50), `_lower_recursive_rule` refactor (~30), `_extract_captures`/`_walk_matcher` updates (~70), `_check_no_capture_after_recursion` (~30).

Tests: ~350 LOC across 9 behavioral cases + 1 integration assertion.

---

## 4. Risk register

| # | Risk | Mitigation |
|---|---|---|
| 1 | Phase 4 walker mis-aligns cursor for anchor-in-tail variants. | §2.4 traces both anchor-in-tail variants through walker + engine. Recursive walker handles multi-hop without code change. Surgical budget if discovered wrong: <30 LOC. |
| 2 | Variant B.1 (anchor-in-tail with PARENT incoming edge) semantics ambiguous. | Documented in §2.6 as "preserve existing single-hop behavior, extend forward only." Test 4.e asserts implementation behavior rather than a hand-derived ground truth. If user flags as wrong, alternative is rejection via new C008 diagnostic — defer. |
| 3 | `_strip_trailing_self_ref` misses an edge form (e.g. `( maybe $self )` wrapped in extra parens, or `$self` without `maybe`). | Pattern matching is structural on the AST, not text. Test 3.h covers the non-trailing case; spec §1 declares non-trailing $self out of scope. Permissive sentinel still fires for those — preserves v0 behavior, no regression. |
| 4 | `set()`-induced order loss in engine `process_child_matcher` masks per-iteration binding. | Per-iteration extraction reads `_mch_iter_blocks` (preserved in iteration order, NOT set-deduped) BEFORE the outer `blocks.extend(set(...))` happens. Order preserved by design. The walker on outer matcher then uses `cursor = len(all_blocks)` to skip past the recursion. |
| 5 | Stonfi v2 integration test breaks. | Outside-recursion captures unchanged. `_mch_has_pure_exit=False` activates new termination logic; for the existing single-hop trace, the new logic runs the step once, finds no next match (block.next_blocks doesn't satisfy any step branch), terminates with `child_matched=True` (since 1 iteration succeeded). DSL count stays at 1, matching hand-written. Same result, cleaner path. |
| 6 | Walker change affects Phase 1/2 tests. | New params `capture_infos` are additive. List-binding initialization affects only list_binding=True captures. Phase 1/2 captures are all scalar (list_binding=False, including in stonfi v2 — `m.swap` and `m.payout` are outside any recursive rule). Initialization branch chooses None for those. Re-run 98 baseline tests gate. |
| 7 | `_check_no_capture_after_recursion` false-positives on legal patterns. | Apply ONLY when `$rule_name` is direct-recursive (per `ctx.resolved.rule_recursion[name]`). Non-recursive rules inline as plain patterns; no recursion-after captures issue. Verified against the existing stonfi_v2 spec (no captures after `$next_hops`). |
| 8 | Re-entry into `_MchRecursiveMatcher.process_child_matcher` on shared instance corrupts `_mch_iter_blocks`. | Engine matches sequentially per `try_build` call. `self._mch_iter_blocks = []` resets at start of each call. No concurrent calls supported. Document in code comment. |
| 9 | Python recursion limit on multi-hop parent chain. | Production matchers ≤10 hops. Python default limit 1000. Negligible. |

---

## 5. Tests & regression strategy

| Step | Command | Expected |
|---|---|---|
| Baseline pre-flight | `python -m pytest tests/mch/ --tb=no -q` | 98 passed (Phase 1/2 baseline). |
| After Phase 4 lowering | full mch suite | 98 + 8 Phase 4 tests + 1 converted structural. |
| After Phase 4 lowering | `test_compiler_structural.py::test_multi_hop_parent_chain_rejected` | NOT present (converted to positive variant). |
| After Phase 3 subclass + walker + strip | full mch suite | All Phase 4 + 9 Phase 3 tests. |
| After Phase 3 subclass + walker + strip | stonfi v2 integration | PASS — outside captures scalar, count parity preserved. |
| Smoke | `python -m pytest tests/test_actions.py --tb=no -q` | Identical to baseline (DSL not wired into production). |

---

## 6. Out of scope (explicit)

- Captures appearing AFTER a recursion in the same sequence (e.g. `op A @a -> $rule -> op B @b`). Rejected with C007.
- Indirect (mutual) recursion. C005 stays.
- Non-trailing `$self` references in rule bodies. Permissive sentinel still fires; behavior undefined for unusual patterns.
- Captures inside `pred`/`predicate` heads.
- Mixed `<-` and `->` interleaved in non-trivial ways (e.g. `op A @a -> op B <- op C -> op D`). Existing v0 silently drops the `<- op C` segment when anchor=A. Phase 4 does NOT improve this; tracked separately if a production matcher needs it.
- Performance: per-iteration walks add O(iterations × walker_depth). Acceptable for v0.

---

## 7. Rollback

- All changes confined to `indexer/events/mch/compiler.py`, `tests/mch/**`, and `docs/superpowers/plans/2026-05-19-mch-future-roadmap.md`.
- No production code edited.
- Commits are atomic per phase (Phase 4 first, then Phase 3) to allow surgical revert via `git revert`.

---

## 8. Definition of done

1. Multi-hop parent chains (2-hop, 3-hop, anchor-head and anchor-in-tail variants B.1, B.2, partial-match-rejection) compile and bind captures correctly.
2. `C006_MULTI_HOP_PARENT_UNSUPPORTED` diagnostic and its check function removed.
3. `_MchRecursiveMatcher` records per-iteration consumed blocks; walker populates list captures.
4. Synthetic 3-iter recursion: `len(m.a) == 3`; parallel peer captures aligned.
5. Zero-iteration recursion: capture is `[]`, not None.
6. Recursion with no pure-exit branches actually iterates >1 time on multi-hop traces (regression for the §3.2 bug).
7. `C007_CAPTURE_AFTER_RECURSION` diagnostic implemented and tested.
8. `list_binding` attribute (NOT `is_list`) used throughout walker.
9. Baseline 98 mch tests + ~17 new tests, all green.
10. `tests/test_actions.py` baseline unchanged.
11. Roadmap document marks Phases 3 and 4 done with brief implementation notes.
