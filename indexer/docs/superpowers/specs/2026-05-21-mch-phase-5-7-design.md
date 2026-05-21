# MCH DSL — Phase 5 + Phase 7 Design

**Date:** 2026-05-21
**Status:** Spec draft (review applied 2026-05-21)
**Scope:** Phase 5 (`ChildrenBlock` semantics at any depth) + Phase 7 (explicit `^` XOR separator with default heuristic). Both phases bundled because they share the `_lower_atom` lowering surface and both target authoring ergonomics + correctness of pattern semantics.

## §1 — Scope

Two surgical phases:

- **Phase 5** — `ChildrenBlock` (`{A; B}`) at any depth in a Sequence binds to its **immediate left sibling** in the lowered matcher chain (mirroring how `_lower_child_chain_from` already handles anchor-direct-children). `maybe {A; B}` (Maybe-wrapped CB) attaches items with `optional=True` to the left sibling — "optional children". Bare `ChildrenBlock` (no left sibling — appearing as Sequence head, inside `Alternative`, or as a rule body root with no chain context) is rejected with new diagnostic `C008_CHILDREN_BLOCK_BARE`.
- **Phase 7** — New `^` separator for `Alternative`. Explicit `^` always lowers to `ExclusiveOrMatcher`. Default `|` lowers via a conservative heuristic: if every branch is concrete (`op X` or `btype Y` head, no `where`/child/parent decorations) and pairwise distinct, lower to `ExclusiveOrMatcher`; otherwise `OrMatcher`. Mixing `|` and `^` at the same nesting level is a parser error `P010_ALTERNATIVE_SEPARATOR_MIX`.

Out of scope (durable for this cycle): recursion, capture extraction, codegen field types, selectivity-based auto-anchor, multi-file namespace, production wiring, engine-level matcher class changes (`ExclusiveOrMatcher` already exists at `basic_matchers.py:140`).

## §2 — Phase 5: ChildrenBlock attach-to-left

### Current state

- `compiler.py:578-616 _lower_atom` for `ChildrenBlock` returns `OrMatcher([...items])` — silently wrong; means "any one of B, C matches" instead of "B and C are both present as children".
- `compiler.py:483-516 _lower_child_chain_from` handles the anchor-direct-children case correctly via `children_matchers=[B, C]` when the FIRST downstream atom after the anchor is a `ChildrenBlock`.

The gap is everything else: mid-chain ChildrenBlock, ChildrenBlock inside Alternative/Maybe, ChildrenBlock as a rule body root.

### New rule

Attach-to-left applies in TWO places (both walk a Sequence-tail-like atom list):

1. `_lower_atom(Sequence)` — for nested Sequences (RuleRef body, Alternative branch inner, Maybe inner).
2. `_lower_child_chain_from` `downstream_atoms` walk and `_lower_parent_chain_from` `parent_atoms` walk — for the top-level matcher anchor-relative path. Replace `_chain(_lower_atom(a) for a in ...)` with the iteration below so mid-chain ChildrenBlocks attach correctly when they're NOT adjacent to the anchor. The anchor-adjacent ChildrenBlock fast path at `_lower_child_chain_from:506-512` is kept as-is (it produces `children_ms` for the anchor's `children_matchers` directly via the compiled-matcher return tuple, not via the chained matcher's `children_matchers`).

Algorithm for the iteration (call it `_lower_chain_with_children_attach`):

Given an ordered list of `(edge, atom)` pairs (head's edge is conventionally CHILD; for parent chain, all edges are PARENT):

- `previous_lowered = None`; `head_matcher = None`.
- For each `(edge, atom)`:
  - If atom is a `ChildrenBlock`:
    - If `previous_lowered is None`: error `C008_CHILDREN_BLOCK_BARE` (no left sibling — bare).
    - Lower each item in the ChildrenBlock with `_lower_atom` individually.
    - Append those lowered items to `previous_lowered.children_matchers` (initialize to fresh list if `None`; otherwise extend in source order).
    - Do NOT update `previous_lowered` — subsequent atoms continue chaining off the same preceding matcher.
    - The ChildrenBlock is a constraint on the preceding matcher, not a node in the chain.
  - Otherwise:
    - Lower the atom via `_lower_atom`.
    - If `previous_lowered is None`: set `head_matcher = lowered`.
    - Else: attach to `previous_lowered` via `_attach_as_child` if `edge is Edge.CHILD`, `_attach_as_parent` if `edge is Edge.PARENT`.
    - Update `previous_lowered = lowered`.
- Return `head_matcher`.

For `_lower_atom(Sequence)`: feed `[(Edge.CHILD, p.head)] + list(p.tail)` (head conventionally CHILD; PARENT-edged tail atoms are honored mid-iteration). Note: `_lower_atom(Sequence)` today (`compiler.py:587-592`) ignores `_edge` and always calls `_attach_as_child`. The new algorithm fixes this pre-existing latent bug. Patterns relying on the always-child behavior inside RuleRef bodies will change semantics — see Risk register.

For `_lower_child_chain_from`: keep the anchor-adjacent ChildrenBlock fast path (lines 506-512). Replace the line-513 `_chain` fallback with the new algorithm, feeding `[(Edge.CHILD, a) for a in downstream_atoms]`. The anchor-adjacent CB is consumed BEFORE the new algorithm runs, so no double-attach to the same `children_matchers` list at the anchor.

For `_lower_parent_chain_from`: replace lines 569-575 (chain via `_attach_as_parent`) with the new algorithm using `[(Edge.PARENT, a) for a in parent_atoms]`. ChildrenBlocks encountered in the parent chain attach to the immediately-left parent-chain matcher, not to the anchor.

### Bare ChildrenBlock rejection

`_lower_atom` for `ChildrenBlock` itself (i.e. reached as a standalone atom outside Sequence-tail context AND outside `maybe {...}` tail context) emits `C008_CHILDREN_BLOCK_BARE`. Covers:

- ChildrenBlock as Sequence head (no left sibling).
- ChildrenBlock as branch of `Alternative` (no Sequence context).
- Bare `maybe {...}` (Maybe wrapping a ChildrenBlock with no left sibling — i.e. `maybe` is itself the Sequence head or appears in Alternative/standalone position).
- ChildrenBlock as the root of a `rule` body (when reached via `RuleRef` expansion).

The diagnostic carries the ChildrenBlock's span. Resolution-time vs compile-time: emit during compilation in `_lower_atom`. Resolver does NOT pre-check rule body roots; the error surfaces when a matcher that uses the rule attempts to compile.

### Maybe-wrapped ChildrenBlock (optional children)

When a `maybe {...}` appears as a Sequence tail atom with a left sibling, the inner ChildrenBlock items lower with `optional=True` and attach to the left sibling's `children_matchers`. Semantics: "left sibling may or may not have these children — match succeeds regardless".

Algorithm extension (inside `_lower_chain_with_children_attach`): when the current atom is `Maybe(ChildrenBlock(items))` AND `previous_lowered is not None`, lower each item with `_lower_atom` then set `item.optional = True` on each result, then extend `previous_lowered.children_matchers`. Like the bare-CB case, `previous_lowered` is NOT updated — the maybe-CB is a constraint, not a chain link.

`maybe {...}` with `previous_lowered is None` → `C008`. Same bare rule.

Engine: `process_children_matchers` (`basic_matchers.py:89-92`) honors `optional` per child item — items with `optional=True` don't fail the parent's match when no child block satisfies them.

### Anchor-direct-children path interaction

`_lower_child_chain_from:506-512` runs BEFORE the new mid-chain rule kicks in. It consumes the first ChildrenBlock when adjacent to the anchor and produces `children_ms` for the anchor's `children_matchers` directly via the function's return tuple (not via attach-to-left). The new algorithm replaces only the `_chain` fallback (line 513) and the `_lower_parent_chain_from` chain (lines 569-575). At the anchor level: the anchor's `children_matchers` has exactly ONE source — either the anchor-adjacent fast path OR (if no anchor-adjacent CB exists) nothing. Mid-chain ChildrenBlocks attach to non-anchor matchers via the new algorithm. No double-source merge at any single matcher's `children_matchers` list.

If a pattern has BOTH anchor-adjacent CB AND mid-chain CB (e.g. `op A @a -> {B; C} -> op X -> {D; E}`): anchor A gets `children_matchers=[B, C]` from the fast path; X gets `children_matchers=[D, E]` from the new algorithm. Disjoint sources.

### Pattern examples

| Pattern | Engine result |
|---|---|
| `op A @a -> {op B; op C}` (anchor direct children — v0 fast path) | A with `children_matchers=[B, C]` |
| `op A @a -> op X -> {op B; op C}` | A→X (child); X has `children_matchers=[B, C]` |
| `op A -> {op B; op C} -> op D` | A has `children_matchers=[B, C]` AND child_matcher=D |
| `op A -> {op B; op C} -> {op D; op E}` (two back-to-back CBs) | A has `children_matchers=[B, C, D, E]` (extended in source order) |
| `op A @a -> {B;C} -> op X -> {D;E}` (anchor-adjacent + mid-chain CB) | A `children_matchers=[B,C]`; X `children_matchers=[D,E]` |
| `op A <- op B -> {op C; op D}` | A's parent=B; B has `children_matchers=[C, D]` (parent chain mid-CB attach via new algorithm) |
| `op A -> maybe {op B; op C}` | A has `children_matchers=[B(optional=True), C(optional=True)]` |
| `(op A \| {op B; op C})` | `C008_CHILDREN_BLOCK_BARE` |
| `maybe {op B; op C}` (no left sibling) | `C008_CHILDREN_BLOCK_BARE` |
| `{op B; op C} -> op A` | `C008_CHILDREN_BLOCK_BARE` |
| `rule r = {A; B}` referenced by bare `$r` (no left sibling) | `C008_CHILDREN_BLOCK_BARE` |
| `rule r = {A; B}` referenced as `op X -> $r` | `C008_CHILDREN_BLOCK_BARE` (RuleRef expansion reveals CB to `_lower_atom`, no attach-to-left at expansion site — v0 limitation, see notes) |

### Capture handling

`{op B @b; op C @c}` items participate in the positional capture walker exactly as if they were direct children of the preceding matcher. Phase 1's `_walk_matcher` (`compiler.py:425-427`) recurses into `children_matchers` AFTER walking `child_matcher` (line 423). No walker change.

**Walker order note**: For `op A @a -> {B @b; C @c} -> op D @d`, A has `child_matcher=D` and `children_matchers=[B, C]`. Walker order: A → D → B → C (NOT A → B → C → D). Captures `m.a, m.b, m.c, m.d` bind by capture-name lookup (positional within each matcher's own `_mch_capture_name`), so name binding is independent of walker traversal order; only cursor advancement is affected. Test 1 verifies binding correctness via capture-name assertions, not by walker traversal order.

### Dual `child_matcher` + `children_matchers` consume semantics

For `op A -> {B; C} -> op D`, A has both `child_matcher=D` and `children_matchers=[B, C]` populated. Engine `process_child_matcher` (`basic_matchers.py:74-87`) runs `child_matcher` and `children_matchers` in INDEPENDENT loops over the SAME `block.next_blocks` — no shared consume state. If D's head overlaps with B or C, the same child block can satisfy both matchers (rare in practice — distinct opcodes by design). Authors writing overlapping patterns should be aware; no compile-time validation in v0.

### RuleRef + ChildrenBlock body — v0 limitation

`rule r = {A; B}` defines a CB as the rule body. Any reference to `$r` expands via `_lower_atom(RuleRef) → _lower_atom(target.pattern)` which hits the bare-CB branch and raises `C008`. Even contexts where attach-to-left would be safe (`op X -> $r`) fail because the new attach-to-left algorithm operates on AST nodes (Sequence tail), not on the post-expansion matcher tree.

**Workaround**: inline the CB at the use site: `op X -> {A; B}`. Or wrap the CB in a sub-pattern that includes a dummy left sibling — not supported in v0 either.

**Future phase**: transparent RuleRef expansion that preserves attach-to-left context is out of scope.

### Files touched

- `indexer/events/mch/compiler.py`:
  - Add `_lower_chain_with_children_attach(pairs, ctx)` helper (handles bare CB and Maybe-wrapped CB attach-to-left).
  - Rewrite `_lower_atom(Sequence)` to call it with `[(Edge.CHILD, head)] + list(tail)`.
  - Rewrite `_lower_atom(ChildrenBlock)` to emit `C008_CHILDREN_BLOCK_BARE`.
  - Rewrite `_lower_atom(Maybe)` to detect `Maybe(ChildrenBlock)` as bare-when-not-in-chain-context and emit `C008`; pass-through to `_lower_atom(inner)` otherwise.
  - Replace `_lower_child_chain_from` line-513 `_chain(...)` fallback with helper call over downstream_atoms (anchor-adjacent fast path at 506-512 kept).
  - Replace `_lower_parent_chain_from` chain build (lines 569-575) with helper call over parent_atoms.
- `indexer/events/mch/diagnostics.py` — register `C008_CHILDREN_BLOCK_BARE`.

## §3 — Phase 7: Explicit `^` XOR separator + default heuristic

### Lexer

- Add `TokenKind.CARET = "^"` in `tokens.py`.
- Lexer recognizes single-char `^` and emits a `CARET` token. No multi-char extension (`^^` etc. unsupported; would lex as two CARETs and parser would error appropriately).

### AST

`ast_.Alternative` gains an `exclusive: bool = False` field. Default keeps existing behavior; parser sets `exclusive=True` when the alternative was built with `^` separators.

```python
@dataclass(frozen=True)
class Alternative(PatternExpr):
    branches: tuple[PatternExpr, ...]
    exclusive: bool  # set by parser; True if `^` separators used
    span: Span
```

**Field-order note**: `exclusive` must precede `span` because `span` has no default. Frozen dataclasses still enforce non-default-after-default ordering. All call sites that construct `Alternative(branches=..., span=...)` must pass `exclusive=False` (or use kwargs).

### Parser

Current alternative parse loop reads `PIPE` between branches. New behavior:

1. Parse first branch.
2. Save `s.i`, skip newlines, then peek. If `PIPE` or `CARET`: remember separator kind, restore-or-keep position to consume it. Otherwise: restore `s.i`, alternative ends (single branch — returned as non-alternative atom). Newline save/restore MUST apply to both separator kinds — symmetric handling.
3. Parse remaining branches in a loop, consuming the same separator token each time. After consuming separator, parse a branch — failure to parse one is a generic parser error (no special diagnostic; existing parser error path handles it). If a DIFFERENT separator appears mid-list, capture its span via `s.tokens[s.i]` BEFORE advancing `s.i`, emit `P010_ALTERNATIVE_SEPARATOR_MIX`, then halt the alternative (return what's been parsed so far OR raise `ParseError`, matching the existing parser error-recovery style).
4. Build `Alternative(branches=..., exclusive=(separator is CARET), span=...)`.

**Degenerate case**: `(op A ^)` — `^` consumed, parser attempts to parse next branch, fails on `)`. Generic parser error fires. Acceptable.

**Single-branch case**: `(op A)` — no separator peeked, returned as `op A` directly (not wrapped in `Alternative`). Matches current behavior.

Nested groups in parens are independent — each `(` starts a fresh separator decision.

### Compiler lowering

`_lower_atom` for `Alternative`:

```python
if isinstance(p, ast_.Alternative):
    lowered_branches = [_lower_atom(b, ctx) for b in p.branches]
    if p.exclusive:
        # Validation pass (W202)
        _check_redundant_exclusive_heads(p, lowered_branches, ctx)
        return ExclusiveOrMatcher(lowered_branches)
    if _branches_eligible_for_exclusive_heuristic(p.branches, lowered_branches):
        return ExclusiveOrMatcher(lowered_branches)
    return OrMatcher(lowered_branches)
```

#### Heuristic eligibility

`_branches_eligible_for_exclusive_heuristic` returns True iff:

1. Every AST branch is a bare `Node` (not Sequence, Maybe, Alternative, ChildrenBlock, RuleRef).
2. Every AST branch's head is `OpHead` (concrete int opcode) or `BtypeHead` (concrete identifier).
3. No branch has a `where` predicate.
4. All concrete keys are pairwise distinct within their type (opcode-vs-opcode and btype-vs-btype; cross-type collisions impossible since head types are disjoint).

Captures on branches do NOT affect eligibility (they bind names but don't change `test_self` behavior).

If any condition fails, fall through to `OrMatcher`.

Conservative: when in doubt, OrMatcher. Author who wants ExclusiveOrMatcher in doubt-cases must say `^`.

#### W202 validation when `exclusive=True`

After lowering, check pairwise concrete head distinctness on the original branches:

- If two branches are both bare concrete nodes with the same opcode/btype head (regardless of `where` clauses, captures), emit `W202_EXCLUSIVE_OR_REDUNDANT_HEADS` warning. Non-blocking — diagnostic is added to the bag but lowering continues to produce `ExclusiveOrMatcher`.
- Branches that are non-concrete (Sequence, Maybe, RuleRef, nested Alternative) are ignored by this check — can't analyze statically. Known gap: `(op A ^ (op A | op B))` won't warn even though outer-branch-1 head A overlaps with inner-OrMatcher-branch-1 head A. Documented in risk register R9.

### XOR captures (same-name across branches)

In an exclusive alternative, only one branch's matchers run successfully. Captures bound in the losing branch(es) retain their default value (`None` for scalar captures, `[]` for list captures under recursion).

Example: `(op A @x ^ op B @x)`. Tree matches opcode A → `m.x = <A block>`. Tree matches opcode B → `m.x = <B block>`. No tree can produce both (sum-check rejects).

Different-name captures across XOR branches: `(op A @a ^ op B @b)`. Tree matches A → `m.a = <A block>`, `m.b = None`. Tree matches B → `m.a = None`, `m.b = <B block>`.

No new diagnostic. Generated `Match` dataclass field types stay nullable for scalars (already the case; captures under non-recursive matchers emit `Block | None`).

### Diagnostics added

- `P010_ALTERNATIVE_SEPARATOR_MIX` — parser, error, surfaces during `parse()`.
- `W202_EXCLUSIVE_OR_REDUNDANT_HEADS` — compiler, warning, surfaces during `_lower_atom` of Alternative.

### Files touched

- `indexer/events/mch/tokens.py` — `CARET` token.
- `indexer/events/mch/lexer.py` — emit `CARET` on `^`.
- `indexer/events/mch/parser.py` — alternative-separator detection + `P010`.
- `indexer/events/mch/ast_.py` — `Alternative.exclusive: bool = False`.
- `indexer/events/mch/compiler.py` — branch lowering + heuristic + `W202`.
- `indexer/events/mch/diagnostics.py` — `P010`, `W202` registration.

## §4 — Tests (TDD)

Behavioral tests in `tests/mch/test_compiler_behavioral.py`. Structural/diagnostic tests in `tests/mch/test_compiler_structural.py`. Each test written RED first (failure verified) before code change.

### Phase 5

1. `test_children_block_mid_chain_binds_to_left` — `op A @a -> op X -> {op B @b; op C @c}`; synthetic tree A→X with X's children={B, C}; assert match, `m.b/m.c` bound correctly.
2. `test_children_block_after_anchor_unchanged` — regression `op A @a -> {op B; op C}` (anchor-adjacent fast path).
3. `test_children_block_mid_chain_continues_after` — `op A -> {op B; op C} -> op D`; tree: A's children include B, C; A's descendant chain reaches D.
4. `test_children_block_inside_alternative_branch_rejected` — `(op A | {op B; op C})` → `C008`.
5. `test_children_block_inside_bare_maybe_rejected` — `maybe {op B; op C}` (bare, no left sibling) → `C008`.
5b. `test_maybe_wrapped_children_block_in_chain_attaches_optional` — `op A -> maybe {op B; op C}`; synthetic tree A with no children → match succeeds (children optional). Tree A with children B, C → match succeeds. Tree A with only B → match succeeds (per-item optional). Asserts engine treats items as optional.
6. `test_children_block_as_sequence_head_rejected` — `{op B; op C} -> op A` → `C008`.
7. `test_children_block_inside_rule_body_root_rejected` — `rule r = {A; B}` referenced by `$r` → `C008`.
7b. `test_rule_ref_to_children_block_in_sequence_rejected` — `rule r = {A; B}` referenced as `op X -> $r` → `C008` (documents v0 limitation).
8. `test_children_block_after_parent_edge` — `op A <- op B -> {op C; op D}`; anchor=A, parent chain produces B with `children_matchers=[C, D]`; assert tree: B is A's parent, B has children C, D.
8b. _(removed — replaced by test 5b, which inverts the expected behavior per user decision)_
8c. `test_two_back_to_back_children_blocks` — `op A -> {op B; op C} -> {op D; op E}`; A has `children_matchers=[B, C, D, E]` in source order.
8d. `test_anchor_adjacent_plus_mid_chain_children_blocks` — `op A @a -> {B; C} -> op X -> {D; E}`; A `children_matchers=[B,C]`, X `children_matchers=[D,E]`; disjoint sources.
8e. `test_sequence_inside_rule_body_with_parent_edge` — regression guard: rule body that is a Sequence containing PARENT edge no longer always uses `_attach_as_child`. Pick an existing pattern that exercises this if any; otherwise document that no current pattern uses PARENT-edged Sequence inside a RuleRef body and the change is forward-looking.

### Phase 7

9. `test_caret_separator_lowers_to_exclusive_or` — structural; `(op A ^ op B)` → `ExclusiveOrMatcher`.
10. `test_pipe_separator_concrete_distinct_lowers_to_exclusive_or` — structural; `(op A | op B)` heuristic fires → `ExclusiveOrMatcher`.
11. `test_pipe_separator_with_predicate_lowers_to_or` — `(op A | op A where p)` → `OrMatcher`.
12. `test_pipe_separator_nested_lowers_to_or` — `(op A | (op B -> op C))` → `OrMatcher`.
13. `test_caret_with_predicate_still_exclusive_or` — `(op A ^ op A where p)` → `ExclusiveOrMatcher` (explicit overrides heuristic).
14. `test_nested_caret_outer_with_inner_pipe` — `(op A ^ (op B | op C))` → outer `ExclusiveOrMatcher`, inner `OrMatcher`.
15. `test_mixed_separators_same_level_rejected` — `(op A ^ op B | op C)` → `P010`.
16. `test_caret_redundant_heads_warns` — `(op A ^ op A)` → `W202`.
17. `test_exclusive_or_behavioral_double_match_rejects` — synthetic tree where both branches' heads collide on one block; `ExclusiveOrMatcher.test_self` returns False.
18. `test_caret_inside_recursive_rule` — `rule r = (step ^ maybe $r)` lowers cleanly without crash; asserts the alternative lowers to `OrMatcher` (not `ExclusiveOrMatcher`) since `maybe $r` is non-concrete (Phase 3 regression guard).
18b. `test_xor_same_name_capture_loser_branch_default` — `(op A @x ^ op B @x)`; synthetic tree with opcode A only; `m.x = <A block>`. Synthetic tree with opcode B only; `m.x = <B block>`. Confirms loser branch leaves @x at default.
18c. `test_xor_different_name_captures_only_winner_set` — `(op A @a ^ op B @b)`; tree with A → `m.a` set, `m.b is None`. Tree with B → `m.a is None`, `m.b` set.

### Regression

- Full mch test suite: 98 baseline + 17 Phase 3/4 = **115 tests** must stay green.
- `tests/test_actions.py`: **123 tests** must stay green.

## §5 — Out of scope

- Recursion changes (Phase 3).
- Capture extraction changes (Phase 1).
- Codegen field-type precision (Phase 9).
- Selectivity-based auto-anchor (Phase 6).
- Engine-level (`basic_matchers.py`) edits — `ExclusiveOrMatcher` already implements the required `sum == 1` semantics.
- Multi-file namespace (Phase 8).
- Production wiring (Phase 10/11).

## §6 — Risk register

- **R1 (low) — Anchor-direct-children path overlap.** `_lower_child_chain_from:506-512` consumes the anchor-adjacent ChildrenBlock before mid-chain rule kicks in. Test 2 guards regression.
- **R2 (low) — Heuristic firing on pattern semantically wanting `OrMatcher`.** Heuristic only fires on bare concrete nodes with distinct heads; in that case `OrMatcher` and `ExclusiveOrMatcher` are behaviorally equivalent (at most one branch can match). Safe.
- **R3 (low) — `^` token collides with future stitch syntax.** Spec §11 (stitch) is out-of-scope and doesn't use `^`. `^` is single-char, isolated; future stitch syntax can avoid it.
- **R4 (low) — Heuristic in nested groups.** ChildrenBlock items separated by `;`, not `|`/`^`; items themselves may be Alternatives. Heuristic applies recursively to inner Alternatives — no special case needed.
- **R5 (low) — Capture walker traversal order through new children_matchers.** Phase 1's `_walk_matcher` already recurses `children_matchers` (line 425-427) AFTER walking `child_matcher` (line 423). New rule appends to `children_matchers` in source order; walker sees items in append order. Capture binding is by name not position, so walker order doesn't affect correctness. Test 1 verifies binding correctness via `m.b/m.c` name assertions, not walker order.
- **R6 (medium) — Sequence-inside-`_lower_atom` edge handling change.** Current `_lower_atom(Sequence)` (lines 587-592) always uses `_attach_as_child` regardless of edge. New algorithm honors `Edge.PARENT`. Patterns relying on the latent always-child behavior inside RuleRef bodies / Alternative-branch inners / Maybe inners would change semantics. Mitigation: grep existing test corpus and rule bodies for PARENT edges inside non-top-level Sequences. If any exist, surface in plan as breaking change; otherwise the fix is silent and forward-only. Test 8e captures the regression intent.
- **R7 (low) — `_lower_parent_chain_from` mid-chain CB attach.** New algorithm replaces the chain-build at lines 569-575. The first parent atom is still set as the chain head returned to the anchor's `parent_matcher`. Mid-chain CBs attach to parent-chain matchers, not to the anchor. Test 8 verifies example 4.
- **R8 (low) — Dual child_matcher + children_matchers overlap.** Engine `process_child_matcher:74-87` runs both over same `block.next_blocks` with independent consume state. Overlapping head between `child_matcher` and `children_matchers` items can double-match. Documented in §2 "Dual semantics" subsection; no compile-time validation in v0.
- **R9 (low) — W202 blind to non-concrete branches.** `(op A ^ (op A | op B))` won't warn even though outer A overlaps inner-OrMatcher-can-fire-on-A. Statically un-analyzable in v0. Author must verify intent manually. Future phase could expand W202 detection one level into nested OrMatcher branches.
- **R10 (low) — XOR loser-branch captures stay default.** Documented in §3 "XOR captures" subsection. Caller pattern: check for None before dereferencing. Generated Match dataclass field types remain `Block | None` for scalar captures (already the case in v0).
- **R11 (low) — Maybe-wrapped CB engine semantics.** Per-item `optional=True` on `children_matchers` lets the engine skip when no child matches. Behavior verified by existing test `process_children_matchers` handling of optional items. New test 5b confirms end-to-end.

## §7 — Definition of done

- All 24 new tests pass RED→GREEN (Phase 5: 13 — 1, 2, 3, 4, 5, 5b, 6, 7, 7b, 8, 8c, 8d, 8e; Phase 7: 12 — 9 through 18, 18b, 18c).
- 115 existing mch tests stay green.
- 123 existing actions tests stay green.
- `C008`, `P010`, `W202` emit with correct spans.
- No engine-level (`basic_matchers.py`) edits.
- Spec + implementation plan + commit on `mch-dev` branch.

## §8 — Resolved design decisions (from review)

- **Q1**: Should `op X -> $r` where `r = {A; B}` attach `{A; B}` as children of X (transparent RuleRef expansion)? **v0 answer: no, C008.** Future phase may add transparent expansion via AST-level inlining before lowering.
- **Q2**: Should `op A -> maybe {B; C}` mean "optional children pattern"? **v0 answer: yes — items lower with `optional=True` per-item, attach to left sibling.** Engine `process_children_matchers` honors per-item `optional`; semantics: "left sibling may have any subset of these children, including none".
- **Q3**: Should compiler validate `child_matcher` head ∩ `children_matchers` items heads = ∅? **v0 answer: no validation.** Low-frequency edge case; explicit author responsibility.
- **Q4**: Walker order — should `children_matchers` walk BEFORE `child_matcher` for source-order capture binding? **v0 answer: walker unchanged.** Capture binding is by name; positional order only affects cursor advancement, not test 1 correctness assertion.
- **Q5**: `op A <- op B -> {C; D}` — where do C, D attach? **User decision: parent B.** Spec example 4 + new algorithm in `_lower_parent_chain_from` implements this.
- **Q6**: XOR same-name captures — error, warn, or allow? **User decision: allow.** Loser branch leaves @x at default (None for scalars, [] for list bindings under recursion). Documented in §3 "XOR captures".
