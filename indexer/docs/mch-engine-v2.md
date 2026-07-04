# MCH Engine v2 — Purpose-Built IR Walker

**Status:** Design + normative semantics for the `mch_ir/engine.py` rewrite (branch `mch-engine-v2`).
**Audience:** the implementer of the Python rewrite AND the future C++ port — this document IS the abstract-machine spec both must satisfy.

## 1. Motivation

The v1 IR engine (`indexer/events/mch_ir/engine.py`, 461 lines) reuses `blocks/basic_matchers.py` primitives as its execution core. `try_build` returns a flat consumed-blocks list with no capture info, no iteration structure, no rejection channel, and nondeterministic `set()` ordering — so ~250 lines exist purely to reconstruct information the walk already had and discarded:

| v1 artifact | Why it exists | v2 replacement |
|---|---|---|
| `_walk` positional re-trace (~120 lines) | captures not bound during matching | bind slots during the walk |
| `_IrRecursiveMatcher` journal subclass | iteration grouping lost | iteration frames in MatchContext |
| `_mch_entry_block` / `_mch_result_len` cursor math | can't tell recursion's slice of consumed list | consumed recorded per-frame |
| `_compose_test` monkeypatching | fixed `test_self` on engine classes | node carries predicate/expr slots |
| `IrRootMatcher._rejected` instance flag | no rejection channel in return path | rejection is a return value |
| `_dedup_consumed` | engine reports recursion entry twice | visited-set prevents double-consume |

v2 interprets the IR node graph directly. No `basic_matchers` import. All per-attempt state lives in a `MatchContext`; matcher objects are immutable after load. The walker is the C++ blueprint: slots are ints, edges are node indices, one async boundary.

## 2. Scope

- **Replace**: internals of `indexer/events/mch_ir/engine.py`.
- **Keep untouched**: `loader.py`, `expr.py`, `program.py`, the IR schema (`docs/mch-ir-poc.md`), the frontend, the compiled path, `basic_matchers.py`.
- **API pinned** (existing tests must keep validating the rewrite):
  - `build_matchers(art: Artifact, registries: Registries) -> list[matcher]`
  - each returned matcher exposes: `.name: str`, `.produces: list[str]`, `test_self(block) -> bool`, `async try_build(block) -> list[Block] | None` where `None` = structural mismatch, `[]` = clean rejection (builder/program rejected), non-empty list = `[produced_block, *auto_appended]`.
  - load-time validation kept: unknown predicate / builder name → `IrLoadError`. Additionally v2 validates `parse` message-type names at load (v1 gap — lazy failure at first execution).

Out of scope (follow-ups, do NOT bundle): JSON-Schema loader, single sync evaluator + batched lookups, retiring the compiled path, global candidate ordering by `min_lt` (T21), C++ port.

## 3. Data model

### 3.1 Load-time (immutable)

```
CompiledNode:
    kind:        contract | block_type | pred | any | or | recursive
    test data:   opcode (unsigned 32) | btype | predicate ref
    where:       predicate ref | None          # named `where`
    where_expr:  expr tree | None              # inline where (expr); sync eval
    slot:        int | -1                      # capture slot id
    optional:    bool
    edges:       child: int|-1, children: [int], parent: int|-1,
                 branches: [int], step: int|-1, exit: int|-1
    exclusive:   bool                          # or-nodes only
    strategy:    frontier | cyclic             # recursive nodes only, default
                                               # frontier ([M2R, R11/T28], §4.6b)

CompiledMatcher:
    name, produces
    anchor test: opcode_set (unsigned) | btype set
    root:        node id
    slots:       name -> slot id
    cards:       slot -> one | opt | many      # from the matcher record's captures table
    ownership:   slot -> step | exit | plain   # per recursion node, computed at load
                                               # via reachability over {child, parent,
                                               # children, branches, step, exit} edges
                                               # (v1 `_reachable_captures`)
    builder | build_program | neither
    include_excess, include_bounces            # root-level flags from the record
```

Slot ids and ownership are resolved once at load. The walker never touches capture *names*.

### 3.2 Match-time

```
MatchContext (one per try_build attempt; stack object in C++):
    bindings:  list[(slot, frame, block)]      # append-only between marks
    consumed:  list[Block]                     # traversal order, identity-deduped
    visited:   set[id(Block)]                  # guards consumed dedup
    frame:     current recursion frame tag     # None | (rec_node_id, iter_index) | (rec_node_id, EXIT)
    mark() -> (len(bindings), len(consumed))   # savepoint
    rollback(mark)                             # truncate both lists, rebuild visited tail
```

Rollback must restore `visited` consistently (recompute from `consumed` truncation or keep an undo log). No engine state may live on matcher/node objects — reentrancy requirement (§8, R1).

## 4. Walk algorithm (normative)

Entry: `try_build(anchor_block)`:

1. `test_self(anchor_block)` = anchor test (opcode-set / btype-set membership; opcode compared on the unsigned 32-bit value, block must have `btype == "call_contract"` for opcode anchors). False → return `None`.
2. Fresh `MatchContext`. `match_node(root, anchor_block, ctx)`. False → return `None`.
3. Capture assembly (§5), build phase (§6). Reject → return `[]`.
4. Accept → produced block, `merge_blocks(ctx.consumed)`, apply flows, then root-level excess/bounce auto-append (§4.5) — appended to the RESULT list, after build (v1/legacy `pre_build_auto_append=False` parity). Return `[produced, *auto_appended]`.

### 4.1 `match_node(nid, block, ctx) -> bool`

```
node = nodes[nid]
# ---- test phase (all sync, no I/O) ----
if not kind_test(node, block):        return False
if node.where and not predicates[node.where](block):        return False
if node.where_expr and not eval_where_expr(node.where_expr, block):  return False
# or / recursive dispatch happens in kind_test section below
# ---- bind phase ----
if id(block) not in ctx.visited: ctx.consume(block)
if node.slot >= 0: ctx.bind(node.slot, ctx.frame, block)
# ---- edge phase, fixed order: child, children-set, parent ----
if not match_child_edge(node, block, ctx):     return False
if not match_children_set(node, block, ctx):   return False
if not match_parent_edge(node, block, ctx):    return False
# ---- per-node excess/bounce absorption (§4.5) ----
absorb_aux(block, ctx, excess=True, bounces=True)
return True
```

Kind tests: `contract` → `block.btype == "call_contract" and block.opcode (unsigned) == node.opcode`; `block_type` → `block.btype == node.btype`; `pred` → registered predicate; `any` → always true. `or` and `recursive` override the whole body (§4.4, §4.6).

**Failure discipline**: whenever a sub-attempt fails, the caller rolls back to its savepoint before trying the next alternative or reporting failure. A failed `match_node` may leave the context dirty; ONLY the caller's rollback cleans it.

### 4.2 Child edge (multi-match accumulation — legacy parity, easy to get wrong)

Legacy `process_child_matcher` tries the child matcher on EVERY `next_block` and accumulates ALL full matches:

```
matched_any = False
for cand in block.next_blocks:            # list order, no sorting
    mark = ctx.mark()
    if match_node(node.child, cand, ctx): matched_any = True
    else: ctx.rollback(mark)
edge_ok = child_node.optional or matched_any
```

Not first-win. Two matching children → both consumed. Scalar capture takes the first binding (§5).

### 4.3 Children set (ordered, first-match-wins, consuming)

Legacy `process_children_matchers` parity:

```
working = list(block.next_blocks)          # independent copy; may overlap child-edge consumption
for item_id in node.children:              # declaration order
    for cand in working:                   # list order
        mark = ctx.mark()
        if match_node(item_id, cand, ctx):
            working.remove(cand)           # consume from working set
            break
        ctx.rollback(mark)
    else:
        if not nodes[item_id].optional: return False   # required item unmatched
return True
```

Overlap with the child edge is legal (identity dedup in `ctx.consume` absorbs it — legacy dedups via `merge_blocks(set(...))`).

### 4.4 `or` node

Branch attempts in declaration order; first branch whose FULL subtree matches wins; rollback between attempts.

`exclusive: true` affects the node's **head test only** — count branch head tests (kind + where + where_expr of each branch's root node, no edges), exactly-one required. The head test is consulted ONLY where legacy consults `test_self`: recursion entry tests and nested head counts. When the or-node is matched as an edge target, legacy goes straight to `try_build` (which `ExclusiveOrMatcher` inherits from `OrMatcher` without consulting `test_self`) — so the exclusivity gate is **inert there**, and v2 must NOT apply it before branch attempts. `specs/stonfi_v2_swap.mch` depends on this inertness: its hop branches overlap by prefix and must resolve first-match-in-order.

*(Amended after implementation: the original draft gated `match_node` on the count unconditionally — that mispinned legacy, which never consults `test_self` inside `try_build`.)*

### 4.5 Excess / bounce absorption

Two levels, both legacy-parity:

- **Per matched node** (nested): after a node's edge phase succeeds, consume every `next_block` of the matched block that is a call_contract with opcode `0xd53276db` (excess) or `0xffffffff` (bounce). Nested nodes behave as if both flags are always true — v1 built nested `ContractMatcher`s with default flags; IR node records carry no flags.
- **Root matcher record flags** (`include_excess` / `include_bounces`): control the anchor block's OWN aux siblings, appended to the *result list* AFTER a successful build (never before — a rejection must not have to strip them). These auto-appended blocks are returned alongside the produced block, NOT merged into it.

Note the subtle difference inherited from legacy: nested absorption puts aux blocks into `consumed` (they get merged); root-level appends them to the returned list only.

### 4.6 `recursive` node (frontier walk, T18 semantics)

Test phase: the STEP node's head test against the entry block (legacy `RecursiveMatcher.test_self = repeating.test_self`).

```
frontier = [entry_block]
iter_index = 0
loop:
    next_frontier = []
    stepped = False
    for cand in frontier:                          # list order
        if node.exit >= 0:
            mark = ctx.mark(); ctx.frame = (nid, EXIT)
            if match_node(node.exit, cand, ctx):   # exit tried FIRST per candidate
                return True                        # terminated successfully
            ctx.rollback(mark)
        mark = ctx.mark(); ctx.frame = (nid, iter_index)
        if match_node(node.step, cand, ctx):
            stepped = True
            next_frontier = next_blocks of the blocks consumed by THIS step attempt,
                            in consumption order
            break                                  # one step per round
        ctx.rollback(mark)
    if not stepped:
        # Frontier exhausted. Success iff at least one step landed — REGARDLESS
        # of exit presence (legacy RecursiveMatcher rule; a declared exit that
        # never fires does not fail the recursion, its captures stay empty).
        # Pinned by test_walker.py::test_frontier_exhaustion_leaves_exit_captures_empty.
        return iter_index > 0
    iter_index += 1
```

`ctx.frame` must be restored to the enclosing value on return. Zero iterations + `optional: true` on the recursive node = the standard optional-edge skip at the caller (rollback, continue). Exit present ⇔ pure-exit branches exist (frontend guarantee, schema note 3).

Nested recursion (a recursive node reachable inside another's step): out of scope exactly as v1 — reject at LOAD time with `IrLoadError` (v1 silently degraded; v2 must be explicit).

**Executable spec**: `tests/mch/test_recursion_captures.py` (11 cases) defines binding results — step captures gap-aligned per iteration (unbound in an iteration → `None` appended for that iteration), exit captures bind at most once, post-recursion captures keep binding. v2 must reproduce them through the IR path.

### 4.6b `recursive` node, `strategy: "cyclic"` `[M2R, R11/T28]`

Cyclic descent (spec §10.3): no frontier, no frames, no exit. `step` is the rule-body root whose subgraph carries back-edges to its own root (in-body self-references are plain node refs — the graph cycle IS the recursion, bounded by trace depth since child edges always move down the tree):

```
match_recursive_cyclic(node, entry, ctx):
    # node-level where clauses first, as in 4.6
    return match_node(node.step, entry, ctx)
```

`ctx.frame` is untouched — the body pushes no iteration frames, which is exactly why capture slots reachable inside a cyclic body are rejected at LOAD time (`IrLoadError`; the frontend already rejects them as `C012`). Builders for matchers using cyclic rules are consumed-set driven (`Match._mch_consumed`). Several cyclic recursive nodes may share one body subgraph (one node per reference site; per-site `optional` lives on the recursive node). Existence proof + executable spec: `specs/tonco_swap.mch` (mandatory hop continuation with hop/final branches sharing a prefix — the shape 4.6's exit-first order would terminate at hop 2), `tests/mch/test_cyclic_recursion.py`, `tests/mch/integration/test_tonco_swap.py`.

### 4.7 Parent edge

Single candidate: `block.previous_block`. `None` → edge fails unless the parent node is optional. Full subtree match with rollback.

## 5. Capture assembly

After a successful walk, fold `ctx.bindings` into the env:

- card `one` / `opt`: FIRST binding for the slot wins (matches v1 positional-walk behavior on multi-match child edges); `opt` unbound → `None`; `one` unbound → `None` (loader/frontend guarantee it binds; do not hard-fail).
- card `many`, ownership `step` for recursion node R: one element per iteration frame of R in iteration order; iteration with no binding for the slot contributes `None` (gap alignment). Iterations that consumed nothing contribute nothing.
- card `many`, ownership `exit`: append the EXIT-frame binding (at most one) after the step elements.
- card `many`, ownership `plain` (list capture outside any recursion — possible via same-name captures): all bindings in order.

## 6. Build phase

Identical contract to v1 `IrRootMatcher.build_block`, minus the instance flag:

- **builder**: `SimpleNamespace(**captures)`, `._mch_consumed = ctx.consumed` (already deduped, anchor first). Builder returns `None` → reject.
- **build_program**: env = captures + well-known `consumed` / `anchor` (capture names shadow). `EvalError` → log warning, reject. `reject when` fired → reject.
- **neither**: stub block of `produces[0]` over consumed set (Go parity).
- Accept: `Block(produces_primary, [], data)`, set failed/broken, `merge_blocks(ctx.consumed)`, `apply_flows`.
- **`shape` hook** `[M2R, R06]`: when the matcher record carries `"shape"`, invoke the registered shaper `shaper(produced_block, match) -> None` (sync) as the LAST build step — after `merge_blocks` and `apply_flows`, on every build path. The compiled path calls its shaper at the identical point, so post-surgery block topology is parity-guaranteed. Unknown shaper name → `IrLoadError` at load (`build_matchers`). Region contract: spec §8.7.

Rejection propagates as `try_build -> []`.

## 7. Determinism rules

- Iterate `next_blocks`, frontiers, branches, children in **existing list order** — v2 introduces NO reordering and NO `set()` iteration anywhere. (Global `(min_lt, index)` ordering is T21, deliberately not here: parity with the compiled path first.)
- `ctx.consumed` = traversal order, identity-deduped at consume time. `_mch_consumed` for builders is that list verbatim.
- Two runs over the same tree MUST produce identical bindings and consumed lists (assert in tests).
- Parity comparisons vs legacy/compiled path: compare consumed as SETS and captures field-by-field — legacy's `set()` shuffle makes its list order meaningless.

## 8. Test plan (acceptance)

Green before merge, in this order:

1. **Existing suites unchanged**: `tests/mch_ir/` (68) — `test_engine.py` may adapt asserts that poke v1 internals (`_IrRecursiveMatcher` etc.) but must not weaken behavioral assertions; `tests/mch/integration/test_ir_engine_parity.py` (19 round-trip vs compiled path) must pass UNMODIFIED; full `tests/mch/` (343); `tests/test_actions.py` (123) untouched.
2. **New `tests/mch_ir/test_walker.py`** (adversarial, TDD against this doc):
   - R1 reentrancy: same matcher over two traces sequentially → no state bleed (v1 had a stale-state leak fixed by `_mch_entry_block`; v2 must be immune by construction — assert no mutable attrs on matcher after load).
   - R2 determinism: two identical runs → identical bindings + consumed lists.
   - R3 rollback: or-branch that partially consumes then fails → context restored exactly (bindings, consumed, visited).
   - R4 child-edge multi-match: two matching children both consumed; scalar capture = first.
   - R5 children-set: declaration-order/first-match-wins/working-set-removal; required-unmatched fails; optional tail passes; overlap with child edge dedups.
   - R6 recursion: frontier semantics incl. exit-first-per-candidate, one-step-per-round, gap-aligned step captures, exit bind-once, post-recursion binding — port the 11 `test_recursion_captures.py` cases through IR. Cyclic strategy (§4.6b): back-edge descent, load-time rejection of exit/captures-in-body — `test_cyclic_recursion.py` cases through IR.
   - R7 rejection: builder-None and `reject when` → `[]`, no aux blocks attached, matcher reusable afterwards.
   - R8 excess/bounce: nested absorption into consumed vs root-level append-to-result; `include_*: false` respected at root.
   - R9 exclusive-or: count gate applies ONLY where head tests are consulted (recursion entry / nested head counts — 0 and 2 matching heads fail there); as an edge target the gate is inert and behavior is identical to plain or (first-full-build-wins). See §4.4 amendment.
3. **Parity sweep** (separate harness task): all spec files (`specs/` + `specs_decl/`) × all firing fixtures in `tests/traces/` — v2 vs compiled path, field-level, zero divergences.

## 9. Follow-ups enabled (not in this change)

JSON-Schema loader validation; single sync evaluator + 3-phase batched lookups; retiring the compiled path (frontend emits IR only, one engine per host); T21 global ordering; C++ port (translate `CompiledNode`/`MatchContext`/`match_node` literally).
