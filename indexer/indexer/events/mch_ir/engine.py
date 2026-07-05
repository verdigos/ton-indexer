"""Purpose-built IR walker: node records -> CompiledNode graph -> runnable matchers.

v2 engine (docs/mch-engine-v2.md). Interprets the IR node graph directly — no
`blocks/basic_matchers.py` primitives — while reproducing the legacy engine's
observable semantics:

- child edge: multi-match accumulation over next_blocks (spec §4.2);
- children set: declaration order, first-match-wins, consuming working set (§4.3);
- or: first branch whose full subtree matches; `exclusive` changes the node's
  HEAD TEST only (branch head-test count == 1) and is inert when the node is
  matched as an edge target (§4.4, amended — see below);
- recursive: frontier walk with exit-tried-first-per-candidate and one step
  per round (§4.6, T18 semantics);
- excess/bounce absorption on two levels: every matched nested node consumes
  its excess/bounce children; the root's own aux siblings are appended to the
  result list after a successful build, per record flags (§4.5).

Captures bind DURING the walk (slot ids resolved at load), so there is no
positional re-trace: scalar cards take the first binding, list cards assemble
from per-iteration recursion frames with gap alignment (§5). All per-attempt
state lives in a _MatchContext; matcher objects are immutable after load and
therefore reentrant. Failure discipline: a failed sub-attempt may leave the
context dirty — only the caller's rollback cleans it (§4.1).

Determinism: candidates, branches and children are iterated in existing list
order; `ctx.consumed` is traversal order, identity-deduped at consume time.
No set() is ever iterated (§7).

Known deliberate deviations from the spec doc (legacy parity wins, per the
parity suite):

- §4.6 states that frontier exhaustion fails when an exit is declared; the
  legacy engine succeeds whenever at least one step landed regardless of exit
  presence (basic_matchers.py RecursiveMatcher, pinned by
  test_frontier_exhaustion_leaves_exit_captures_empty).
- §4.4 states `exclusive` gates match_node before the branch attempts; legacy
  ExclusiveOrMatcher overrides only test_self, which try_build never consults,
  so the count gate applies ONLY where the head test is consulted (recursion
  entry, nested head counts). specs/stonfi_v2_swap.mch depends on this
  ("ExclusiveOr elsewhere is try_build-inert" — its hop branches overlap by
  prefix and must resolve first-match-in-order).

Unlike the Go POC, a predicate / builder / parse-message-type name missing from
the host registries is a load-time IrLoadError, not an always-true stub. Nested
recursion (a recursive node reachable from another's step subgraph) is also an
IrLoadError (v1 silently degraded).
"""
from __future__ import annotations

import logging
from types import SimpleNamespace

from indexer.events.blocks.core import Block
from indexer.events.mch.registry import Registries
from indexer.events.mch_ir.expr import EvalError, eval_where_expr
from indexer.events.mch_ir.loader import Artifact, IrLoadError
from indexer.events.mch_ir.program import BuildProgram, apply_flows

logger = logging.getLogger(__name__)

_EXCESS_OPCODE = 0xD53276DB
_BOUNCE_OPCODE = 0xFFFFFFFF
_AUX_OPCODES = frozenset({_EXCESS_OPCODE, _BOUNCE_OPCODE})
# Frame tag marker for exit-branch bindings: frames are (rec_nid, iter_index)
# for step iterations and (rec_nid, _EXIT) for the exit attempt; iter_index is
# an int, so the string can never collide.
_EXIT = "exit"


def build_matchers(art: Artifact, registries: Registries) -> list["IrMatcher"]:
    """Compile the artifact's matcher records once and return the matchers in
    registration order. Every element exposes `.name`, `.produces`,
    `test_self(block)` and `async try_build(block)` (None = structural
    mismatch, [] = clean rejection, non-empty = [produced, *auto_appended])."""
    for i, node in enumerate(art.nodes):
        if node["kind"] == "pred" and node["pred"] not in registries.predicates:
            raise IrLoadError(f"node {i}: predicate {node['pred']!r} is not registered")
        if "where" in node and node["where"] not in registries.predicates:
            raise IrLoadError(f"node {i}: predicate {node['where']!r} is not registered")
    for m in art.matchers:
        anchor = m["anchor"]
        if anchor.get("kind") == "pred" and anchor["pred"] not in registries.predicates:
            raise IrLoadError(
                f"matcher {m['name']!r}: anchor predicate {anchor['pred']!r} "
                f"is not registered"
            )
        if anchor.get("kind") == "mixed":
            for br in anchor["branches"]:
                if "where" in br and br["where"] not in registries.predicates:
                    raise IrLoadError(
                        f"matcher {m['name']!r}: anchor branch predicate "
                        f"{br['where']!r} is not registered"
                    )
        if "builder" in m and m["builder"] not in registries.builders:
            raise IrLoadError(
                f"matcher {m['name']!r}: builder {m['builder']!r} is not registered"
            )
        if "shape" in m and m["shape"] not in registries.shapers:
            raise IrLoadError(
                f"matcher {m['name']!r}: shaper {m['shape']!r} is not registered"
            )
        for stmt in m.get("build_program") or ():
            if stmt.get("s") == "parse":
                for t in stmt["types"]:
                    if t not in registries.message_types:
                        raise IrLoadError(
                            f"matcher {m['name']!r}: parse message type {t!r} "
                            f"is not registered"
                        )

    compiled = [IrMatcher(record, art.nodes, registries) for record in art.matchers]
    return [compiled[mi] for mi in art.registration_order]


def _edge_refs(rec: dict):
    """Node ids referenced by a node record, in a fixed order (determinism)."""
    if "child" in rec:
        yield rec["child"]
    yield from rec.get("children", ())
    if "parent" in rec:
        yield rec["parent"]
    yield from rec.get("branches", ())
    if "step" in rec:
        yield rec["step"]
    if "exit" in rec:
        yield rec["exit"]


class CompiledNode:
    """One IR node record resolved for execution: predicate refs bound to
    callables, capture name to a slot id, edges to local node ids (-1 = absent).
    Immutable after load; carries zero match state."""

    __slots__ = ("kind", "opcode", "btype", "pred", "where", "where_expr",
                 "slot", "optional", "child", "children", "parent",
                 "branches", "step", "exit", "exclusive", "strategy")

    def __init__(self, rec: dict, remap: dict[int, int], slots: dict[str, int],
                 registries: Registries):
        self.kind: str = rec["kind"]
        # Hosts compare opcodes on the unsigned 32-bit value (schema rule).
        self.opcode: int = (rec["opcode"] & 0xFFFFFFFF) if "opcode" in rec else -1
        self.btype: str | None = rec.get("btype")
        self.pred = registries.predicates[rec["pred"]] if self.kind == "pred" else None
        self.where = registries.predicates[rec["where"]] if "where" in rec else None
        self.where_expr: dict | None = rec.get("where_expr")
        # Capture names absent from the matcher record's captures table never
        # bind (v1 parity).
        self.slot: int = slots.get(rec.get("capture"), -1)
        self.optional: bool = bool(rec.get("optional", False))
        self.child: int = remap[rec["child"]] if "child" in rec else -1
        self.children: tuple[int, ...] = tuple(remap[c] for c in rec.get("children", ()))
        self.parent: int = remap[rec["parent"]] if "parent" in rec else -1
        self.branches: tuple[int, ...] = tuple(remap[b] for b in rec.get("branches", ()))
        self.step: int = remap[rec["step"]] if "step" in rec else -1
        self.exit: int = remap[rec["exit"]] if "exit" in rec else -1
        self.exclusive: bool = bool(rec.get("exclusive", False))
        # Recursion strategy (§10.3, [M2R, R11/T28]): "frontier" (default) or
        # "cyclic" — the step subgraph is a rule body with back-edges to its
        # own root; matching is plain recursive descent (see _match_recursive).
        self.strategy: str = rec.get("strategy", "frontier")


class _MatchContext:
    """Per-try_build mutable state (spec §3.2): bindings, consumed set with an
    identity guard, the recursion frame journal, and the current frame tag.
    mark()/rollback() give savepoint semantics; rollback restores `visited`
    from the truncated consumed tail (each consumed entry is unique)."""

    __slots__ = ("bindings", "consumed", "visited", "frames", "frame")

    def __init__(self):
        self.bindings: list[tuple] = []   # (slot, frame, block), append-only between marks
        self.consumed: list[Block] = []   # traversal order, identity-deduped
        self.visited: set[int] = set()    # id(block) membership guard (never iterated)
        self.frames: list[tuple] = []     # ("step", nid, iter_index, consumed_any) | ("exit", nid)
        self.frame = None                 # None | (nid, iter_index) | (nid, _EXIT)

    def mark(self) -> tuple[int, int, int]:
        return (len(self.bindings), len(self.consumed), len(self.frames))

    def rollback(self, mark: tuple[int, int, int]) -> None:
        nb, nc, nf = mark
        del self.bindings[nb:]
        for b in self.consumed[nc:]:
            self.visited.discard(id(b))
        del self.consumed[nc:]
        del self.frames[nf:]

    def consume(self, block: Block) -> None:
        if id(block) not in self.visited:
            self.visited.add(id(block))
            self.consumed.append(block)

    def bind(self, slot: int, block: Block) -> None:
        self.bindings.append((slot, self.frame, block))


def _absorb_aux(block: Block, ctx: _MatchContext) -> None:
    """§4.5 nested absorption: a matched node always consumes its excess/bounce
    children (legacy nested matchers carry default-true include flags)."""
    for nb in block.next_blocks:
        if nb.btype == "call_contract" and getattr(nb, "opcode", None) in _AUX_OPCODES:
            ctx.consume(nb)


class IrMatcher:
    """A matcher record compiled to an executable walker: anchor test, local
    CompiledNode pool, slot table with cards, per-recursion capture ownership,
    and the build behavior (builder / build program / Go-parity stub).
    Immutable after load — reentrancy by construction."""

    __slots__ = ("name", "produces", "_registries", "_anchor_opcodes",
                 "_anchor_btypes", "_anchor_branches", "_anchor_pred",
                 "_nodes", "_root", "_slots", "_cards", "_step_slots",
                 "_exit_slots", "_owned_slots", "_builder_name", "_program",
                 "_include_excess", "_include_bounces", "_shaper_name")

    def __init__(self, record: dict, pool: list[dict], registries: Registries):
        self.name: str = record["name"]
        self.produces: list[str] = list(record["produces"])
        self._registries = registries

        anchor = record["anchor"]
        # Mixed-kind / per-branch-`where` anchor group ([M2R, R09]): union
        # membership over branch heads, then the matching branch's predicate.
        # Resolved to (kind, value, pred_callable_or_None) tuples, source order.
        self._anchor_branches: list[tuple[str, object, object]] | None = None
        # Predicate anchor ([M2R, R10]): no prefilter — test_self runs the pred.
        self._anchor_pred = None
        if anchor["kind"] == "pred":
            self._anchor_opcodes = None
            self._anchor_btypes = None
            self._anchor_pred = registries.predicates[anchor["pred"]]
        elif anchor["kind"] == "mixed":
            self._anchor_opcodes = None
            self._anchor_btypes = None
            self._anchor_branches = [
                (
                    "op" if "op" in br else "btype",
                    (br["op"] & 0xFFFFFFFF) if "op" in br else br["btype"],
                    registries.predicates[br["where"]] if "where" in br else None,
                )
                for br in anchor["branches"]
            ]
        elif anchor["kind"] == "opcode_set":
            self._anchor_opcodes = frozenset(op & 0xFFFFFFFF for op in anchor["values"])
            self._anchor_btypes = None
        else:
            self._anchor_opcodes = None
            self._anchor_btypes = frozenset(anchor["values"])

        # Slot ids in captures-table declaration order; walker never sees names.
        self._slots: dict[str, int] = {
            c["name"]: i for i, c in enumerate(record.get("captures", []))
        }
        self._cards: tuple[str, ...] = tuple(
            c["card"] for c in record.get("captures", [])
        )

        # Reachable subgraph of the artifact pool, remapped to local ids in
        # deterministic pre-order (root = 0). Cycles are legal in the schema.
        order: list[int] = [record["root"]]
        remap: dict[int, int] = {record["root"]: 0}
        qi = 0
        while qi < len(order):
            rec = pool[order[qi]]
            qi += 1
            for ref in _edge_refs(rec):
                if ref not in remap:
                    remap[ref] = len(order)
                    order.append(ref)
        self._nodes: tuple[CompiledNode, ...] = tuple(
            CompiledNode(pool[old], remap, self._slots, registries) for old in order
        )
        self._root = 0

        # Recursion capture ownership (v1 _reachable_captures) + the nested
        # recursion load gate.
        step_slots: dict[int, frozenset[int]] = {}
        exit_slots: dict[int, frozenset[int]] = {}
        owned: set[int] = set()
        for nid, node in enumerate(self._nodes):
            if node.kind != "recursive":
                continue
            reach = self._reachable(node.step)
            for r in reach:
                if self._nodes[r].kind == "recursive":
                    raise IrLoadError(
                        f"matcher {self.name!r}: nested recursion is not supported "
                        f"(recursive node reachable from another's step subgraph)"
                    )
            if node.strategy == "cyclic":
                # Cyclic descent (§10.3): the body pushes no iteration frames,
                # so slots inside it could never assemble — the compiler
                # rejects them (C012); reject handwritten artifacts too.
                if any(self._nodes[r].slot >= 0 for r in reach):
                    raise IrLoadError(
                        f"matcher {self.name!r}: captures inside a cyclic "
                        f"recursion body are not supported"
                    )
                step_slots[nid] = frozenset()
                exit_slots[nid] = frozenset()
                continue
            step_slots[nid] = frozenset(
                self._nodes[r].slot for r in reach if self._nodes[r].slot >= 0
            )
            if node.exit >= 0:
                exit_slots[nid] = frozenset(
                    self._nodes[r].slot for r in self._reachable(node.exit)
                    if self._nodes[r].slot >= 0
                )
            else:
                exit_slots[nid] = frozenset()
            owned |= step_slots[nid]
            owned |= exit_slots[nid]
        self._step_slots = step_slots
        self._exit_slots = exit_slots
        self._owned_slots = frozenset(owned)

        self._builder_name: str | None = record.get("builder")
        self._program: BuildProgram | None = (
            BuildProgram(record["build_program"], registries)
            if record.get("build_program") is not None else None
        )
        self._include_excess: bool = record.get("include_excess", True)
        self._include_bounces: bool = record.get("include_bounces", True)
        # `shape` directive (§8.7): validated against registries at load
        # (build_matchers); resolved lazily like builders for symmetry.
        self._shaper_name: str | None = record.get("shape")

    def _reachable(self, start: int) -> list[int]:
        """Local node ids reachable from `start` over every edge kind
        (cycle-safe, deterministic order)."""
        seen = {start}
        out = [start]
        qi = 0
        while qi < len(out):
            n = self._nodes[out[qi]]
            qi += 1
            for ref in (n.child, n.parent, n.step, n.exit, *n.children, *n.branches):
                if ref >= 0 and ref not in seen:
                    seen.add(ref)
                    out.append(ref)
        return out

    # ------------------------------------------------------------------ API

    def test_self(self, block: Block) -> bool:
        """Anchor test only (spec §4.1 step 1); node-level where clauses are
        re-checked inside try_build's walk."""
        if self._anchor_pred is not None:
            # [M2R, R10]: full-scan predicate anchor, no prefilter.
            return bool(self._anchor_pred(block))
        if self._anchor_branches is not None:
            # [M2R, R09]: union membership, then the matching branch's `where`.
            for kind, value, pred in self._anchor_branches:
                if kind == "op":
                    ok = block.btype == "call_contract" \
                        and getattr(block, "opcode", None) == value
                else:
                    ok = block.btype == value
                if ok and (pred is None or pred(block)):
                    return True
            return False
        if self._anchor_opcodes is not None:
            return block.btype == "call_contract" \
                and getattr(block, "opcode", None) in self._anchor_opcodes
        return block.btype in self._anchor_btypes

    async def try_build(self, block: Block) -> list[Block] | None:
        if not self.test_self(block):
            return None
        ctx = _MatchContext()
        # The root skips nested aux absorption: the anchor's own excess/bounce
        # siblings are governed by the record flags and appended to the RESULT
        # list after a successful build, never merged (§4.5 root level).
        if not self._match_node(self._root, block, ctx, absorb=False):
            return None
        captures = self._assemble_captures(ctx)
        try:
            produced = await self._build(block, captures, ctx.consumed)
        except Exception as e:
            logger.error(
                "mch_ir matcher %r: error while building block %r: %s",
                self.name, block, e,
            )
            return []  # §10.7: a raising builder is a clean rejection, not a
                       # structural mismatch — no consumption, no aux append.
        if produced is None:
            return []  # clean rejection: no aux blocks to strip
        result = [produced]
        for nb in block.next_blocks:
            if nb.btype != "call_contract":
                continue
            op = getattr(nb, "opcode", None)
            if (op == _EXCESS_OPCODE and self._include_excess) \
                    or (op == _BOUNCE_OPCODE and self._include_bounces):
                result.append(nb)
        return result

    # ------------------------------------------------- walk (spec §4, sync)

    def _test_head(self, nid: int, block: Block) -> bool:
        """Head test only — kind + where + where_expr, no edges. Used for
        or-exclusivity gating and the recursion entry test (§4.4/§4.6)."""
        node = self._nodes[nid]
        kind = node.kind
        if kind == "contract":
            ok = block.btype == "call_contract" \
                and getattr(block, "opcode", None) == node.opcode
        elif kind == "block_type":
            ok = block.btype == node.btype
        elif kind == "pred":
            ok = bool(node.pred(block))
        elif kind == "any":
            ok = True
        elif kind == "or":
            if node.exclusive:
                ok = sum(1 for b in node.branches if self._test_head(b, block)) == 1
            else:
                ok = any(self._test_head(b, block) for b in node.branches)
        else:  # recursive
            ok = self._test_head(node.step, block)
        if not ok:
            return False
        if node.where is not None and not node.where(block):
            return False
        if node.where_expr is not None and not eval_where_expr(node.where_expr, block):
            return False
        return True

    def _match_node(self, nid: int, block: Block, ctx: _MatchContext,
                    absorb: bool = True) -> bool:
        """Full node match against `block` (§4.1). May leave the context dirty
        on failure — the caller's rollback cleans it."""
        node = self._nodes[nid]
        kind = node.kind
        if kind == "or":
            return self._match_or(node, block, ctx)
        if kind == "recursive":
            return self._match_recursive(nid, node, block, ctx)
        if not self._test_head(nid, block):
            return False
        ctx.consume(block)
        if node.slot >= 0:
            ctx.bind(node.slot, block)
        if not self._match_child_edge(node, block, ctx):
            return False
        if not self._match_children_set(node, block, ctx):
            return False
        if not self._match_parent_edge(node, block, ctx):
            return False
        if absorb:
            _absorb_aux(block, ctx)
        return True

    def _match_child_edge(self, node: CompiledNode, block: Block,
                          ctx: _MatchContext) -> bool:
        """§4.2 multi-match accumulation: EVERY next_block is tried and ALL
        full matches consume — not first-win."""
        if node.child < 0:
            return True
        matched_any = False
        for cand in block.next_blocks:
            mark = ctx.mark()
            if self._match_node(node.child, cand, ctx):
                matched_any = True
            else:
                ctx.rollback(mark)
        return matched_any or self._nodes[node.child].optional

    def _match_children_set(self, node: CompiledNode, block: Block,
                            ctx: _MatchContext) -> bool:
        """§4.3: declaration order, first-match-wins per item, matched
        candidates removed from an independent working copy of next_blocks
        (overlap with the child edge dedups through ctx.consume)."""
        if not node.children:
            return True
        working = list(block.next_blocks)
        for item_id in node.children:
            matched = False
            for cand in working:
                mark = ctx.mark()
                if self._match_node(item_id, cand, ctx):
                    working.remove(cand)
                    matched = True
                    break
                ctx.rollback(mark)
            if not matched and not self._nodes[item_id].optional:
                return False
        return True

    def _match_parent_edge(self, node: CompiledNode, block: Block,
                           ctx: _MatchContext) -> bool:
        """§4.7: single candidate — block.previous_block."""
        if node.parent < 0:
            return True
        prev = block.previous_block
        if prev is not None:
            mark = ctx.mark()
            if self._match_node(node.parent, prev, ctx):
                return True
            ctx.rollback(mark)
        return self._nodes[node.parent].optional

    def _match_or(self, node: CompiledNode, block: Block,
                  ctx: _MatchContext) -> bool:
        """§4.4 (amended): first branch whose FULL subtree matches wins,
        rollback between attempts. `exclusive` and the node's own where clause
        participate in HEAD TESTS only — legacy parity: ExclusiveOrMatcher
        overrides test_self, which OrMatcher.try_build never consults, and
        specs/stonfi_v2_swap.mch relies on overlapping `^` branches resolving
        first-match-in-order (see module docstring)."""
        for bid in node.branches:
            mark = ctx.mark()
            if self._match_node(bid, block, ctx):
                return True
            ctx.rollback(mark)
        return False

    def _match_recursive(self, nid: int, node: CompiledNode, entry: Block,
                         ctx: _MatchContext) -> bool:
        """§4.6 frontier walk, T18 semantics: exit tried FIRST per candidate,
        one step per round, the stepped blocks' next_blocks (in consumption
        order) become the new frontier. Entry gate = the STEP node's head test
        (legacy RecursiveMatcher.test_self) plus the node's own where clauses.

        §4.6b cyclic strategy ([M2R, R11/T28]): no frontier and no frames —
        `step` is the rule-body root whose subgraph contains back-edges to
        itself; one full _match_node descent through the cycle IS the
        recursion, bounded by trace depth (child edges always move down the
        tree). Same-prefix step/final branches disambiguate by ordered
        alternatives looking down the actual chain — the shape the frontier
        walk's exit-first order cannot express."""
        if node.where is not None and not node.where(entry):
            return False
        if node.where_expr is not None and not eval_where_expr(node.where_expr, entry):
            return False
        if node.strategy == "cyclic":
            return self._match_node(node.step, entry, ctx)
        if not self._test_head(node.step, entry):
            return False
        outer_frame = ctx.frame
        try:
            frontier = [entry]
            iter_index = 0
            while True:
                stepped = False
                for cand in frontier:
                    if node.exit >= 0:
                        mark = ctx.mark()
                        ctx.frame = (nid, _EXIT)
                        if self._match_node(node.exit, cand, ctx):
                            ctx.frames.append((_EXIT, nid))
                            return True
                        ctx.rollback(mark)
                    mark = ctx.mark()
                    ctx.frame = (nid, iter_index)
                    if self._match_node(node.step, cand, ctx):
                        stepped = True
                        step_consumed = ctx.consumed[mark[1]:]
                        ctx.frames.append(("step", nid, iter_index, bool(step_consumed)))
                        frontier = [nb for b in step_consumed for nb in b.next_blocks]
                        break
                    ctx.rollback(mark)
                if not stepped:
                    # Frontier exhausted. Legacy rule: success iff at least one
                    # step landed, regardless of exit presence (RecursiveMatcher
                    # in basic_matchers.py; spec §4.6 mispins this — see the
                    # module docstring).
                    return iter_index > 0
                iter_index += 1
        finally:
            ctx.frame = outer_frame

    # ------------------------------------------------ captures & build (§5/§6)

    def _assemble_captures(self, ctx: _MatchContext) -> dict:
        """Fold ctx.bindings into the capture env (§5): scalars take the first
        binding; `many` slots owned by a recursion assemble one element per
        journaled iteration frame (gap-aligned None for unbound iterations)
        plus the exit-frame element when the exit fired; unowned `many` slots
        collect every binding in order."""
        first_any: dict[int, Block] = {}
        first_framed: dict[tuple, Block] = {}
        for slot, frame, b in ctx.bindings:
            if slot not in first_any:
                first_any[slot] = b
            key = (slot, frame)
            if key not in first_framed:
                first_framed[key] = b

        env: dict = {}
        for name, slot in self._slots.items():
            if self._cards[slot] != "many":
                env[name] = first_any.get(slot)
                continue
            if slot not in self._owned_slots:
                env[name] = [b for s, _f, b in ctx.bindings if s == slot]
                continue
            items: list = []
            for entry in ctx.frames:
                if entry[0] == "step":
                    _tag, rid, idx, consumed_any = entry
                    if consumed_any and slot in self._step_slots[rid]:
                        items.append(first_framed.get((slot, (rid, idx))))
                else:  # exit frame: bind-once, None for unbound exit sub-branches
                    rid = entry[1]
                    if slot in self._exit_slots[rid]:
                        items.append(first_framed.get((slot, (rid, _EXIT))))
            env[name] = items
        return env

    def _apply_shape(self, produced: Block, match_obj) -> None:
        """§8.7 `shape` hook: LAST build step, after merge (and flows on
        declarative matchers) — same point as the compiled path, so topology
        parity holds. Shapers are sync `(produced_block, match) -> None` and
        may mutate only the produced block's region."""
        if self._shaper_name is None:
            return
        shaper = self._registries.shapers.get(self._shaper_name)
        if shaper is None:
            raise RuntimeError(
                f"shaper {self._shaper_name!r} is not registered at run time"
            )
        shaper(produced, match_obj)

    async def _build(self, anchor: Block, captures: dict,
                     consumed: list[Block]) -> Block | None:
        """Build phase (§6). Returns the produced block, or None for a clean
        rejection (builder returned None / `reject when` fired / EvalError)."""
        if self._builder_name is not None:
            builder = self._registries.builders.get(self._builder_name)
            if builder is None:
                raise RuntimeError(
                    f"builder {self._builder_name!r} is not registered at run time"
                )
            match = SimpleNamespace(**captures)
            match._mch_consumed = consumed
            produced = await builder(match)
            if produced is None:
                return None
            produced.merge_blocks(consumed)
            self._apply_shape(produced, match)
            return produced

        if self._program is not None:
            env = dict(captures)
            # Well-known env names; a capture of the same name shadows them.
            env.setdefault("consumed", consumed)
            env.setdefault("anchor", anchor)
            try:
                outcome = await self._program.run(env)
            except EvalError as e:
                # Spec §10.7: an expression runtime error is a rejection —
                # never aborts trace processing.
                logger.warning(
                    "mch_ir matcher %r: build evaluation failed (%s); rejecting match",
                    self.name, e,
                )
                return None
            if outcome is None:
                return None
            # A `produces switch` selects the produced btype per branch; otherwise
            # the matcher's single declared produces[0] is used.
            produced = Block(outcome.btype or self.produces[0], [], outcome.data)
            produced.failed = outcome.failed
            produced.broken = outcome.broken
            produced.merge_blocks(consumed)
            apply_flows(produced, outcome.flows)
            self._apply_shape(produced, SimpleNamespace(**env))
            return produced

        # Neither builder nor build program (never emitted by the current
        # frontend): Go-POC stub build over the consumed set.
        produced = Block(self.produces[0], [], None)
        produced.merge_blocks(consumed)
        self._apply_shape(produced, SimpleNamespace(**captures))
        return produced
