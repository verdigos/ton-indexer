"""IR-to-engine assembly: node records -> BlockMatcher graph -> runnable matchers.

Mirrors the Go POC loader structurally (two-pass wiring over a flat node pool;
cycles are legal) and the Python compiler behaviorally:

- node kinds map onto the engine primitives in blocks/basic_matchers.py;
- a matcher record's anchor test replaces its root node's own kind test
  (schema emitter-note 1 — the root node is permissive for opcode-set anchors);
- `where` / `where_expr` compose onto test_self, which try_build re-checks;
- capture binding is a post-hoc positional walk over the wired node graph
  against the consumed-blocks list, a mirror of the compiler's
  _extract_captures/_walk_matcher (including the recursion iteration journal);
- the produced block comes from the registered builder, the build program, or
  a bare stub block (Go parity) — in all cases merged over the consumed set.

Unlike the Go POC, a predicate or builder name missing from the host
registries is a load-time IrLoadError, not an always-true stub.
"""
from __future__ import annotations

import logging
from types import SimpleNamespace

from indexer.events.blocks.basic_matchers import (
    BlockMatcher,
    BlockTypeMatcher,
    ContractMatcher,
    ExclusiveOrMatcher,
    GenericMatcher,
    OrMatcher,
    RecursiveMatcher,
)
from indexer.events.blocks.core import Block
from indexer.events.mch.registry import Registries
from indexer.events.mch_ir.expr import EvalError, eval_where_expr
from indexer.events.mch_ir.loader import Artifact, IrLoadError
from indexer.events.mch_ir.program import BuildProgram, apply_flows

logger = logging.getLogger(__name__)


def build_matchers(art: Artifact, registries: Registries) -> list["IrRootMatcher"]:
    """Wire the artifact's node pool once and return the matchers in
    registration order. Every element is a full BlockMatcher (the configured
    root of its pattern graph) with `.name` and `.produces`."""
    for i, node in enumerate(art.nodes):
        if node["kind"] == "pred" and node["pred"] not in registries.predicates:
            raise IrLoadError(f"node {i}: predicate {node['pred']!r} is not registered")
        if "where" in node and node["where"] not in registries.predicates:
            raise IrLoadError(f"node {i}: predicate {node['where']!r} is not registered")
    for m in art.matchers:
        if "builder" in m and m["builder"] not in registries.builders:
            raise IrLoadError(
                f"matcher {m['name']!r}: builder {m['builder']!r} is not registered"
            )

    root_records = {m["root"]: m for m in art.matchers}

    # Pass 1: allocate one instance per node (a stable object any edge —
    # including a cyclic back-edge — can reference).
    instances: list[BlockMatcher] = [
        _make_instance(node, root_records.get(i), registries)
        for i, node in enumerate(art.nodes)
    ]

    # Pass 2: wire edges and per-node flags/tests.
    for node, inst in zip(art.nodes, instances):
        if "child" in node:
            inst.child_matcher = instances[node["child"]]
        if "parent" in node:
            inst.parent_matcher = instances[node["parent"]]
        if node.get("children"):
            inst.children_matchers = [instances[c] for c in node["children"]]
        if node["kind"] == "or":
            inst.matchers = [instances[b] for b in node["branches"]]
        if node["kind"] == "recursive":
            inst.repeating_matcher = instances[node["step"]]
            if "exit" in node:
                inst.exit_matcher = instances[node["exit"]]
        if node.get("optional", False):
            inst.optional = True
        if "capture" in node:
            inst._mch_capture_name = node["capture"]
        if "where" in node:
            _compose_test(inst, registries.predicates[node["where"]])
        if "where_expr" in node:
            expr = node["where_expr"]
            _compose_test(inst, lambda block, _e=expr: eval_where_expr(_e, block))

    # Pass 3: recursion capture attribution (which names belong to the step vs
    # exit subgraph — drives the walk's per-unrolling list padding, spec §7.2).
    for node, inst in zip(art.nodes, instances):
        if node["kind"] == "recursive":
            inst._mch_step_capture_names = _reachable_captures(art.nodes, node["step"])
            if "exit" in node:
                inst._mch_exit_capture_names = _reachable_captures(art.nodes, node["exit"])

    return [instances[art.matchers[mi]["root"]] for mi in art.registration_order]


def _make_instance(node: dict, root_record: dict | None,
                   registries: Registries) -> BlockMatcher:
    if root_record is not None:
        # The root's own kind/opcode is subsumed by the matcher record's anchor
        # test (multi-value opcode_set groups have a permissive "any" root).
        return IrRootMatcher(root_record, registries)
    kind = node["kind"]
    if kind == "contract":
        return ContractMatcher(opcode=node["opcode"] & 0xFFFFFFFF)
    if kind == "block_type":
        return BlockTypeMatcher(block_type=node["btype"])
    if kind == "pred":
        return GenericMatcher(test_self_func=registries.predicates[node["pred"]])
    if kind == "any":
        return GenericMatcher(test_self_func=lambda _b: True)
    if kind == "or":
        cls = ExclusiveOrMatcher if node.get("exclusive", False) else OrMatcher
        return cls([])
    if kind == "recursive":
        return _IrRecursiveMatcher()
    raise IrLoadError(f"unknown node kind {kind!r}")  # loader pre-validates


def _compose_test(m: BlockMatcher, pred) -> None:
    original = m.test_self

    def composed(block, _orig=original, _p=pred):
        return _orig(block) and _p(block)

    m.test_self = composed  # type: ignore[method-assign]


def _reachable_captures(nodes: list[dict], start: int) -> frozenset[str]:
    """All capture names in the subgraph rooted at `start` (cycle-safe)."""
    seen: set[int] = set()
    out: set[str] = set()
    stack = [start]
    while stack:
        i = stack.pop()
        if i in seen:
            continue
        seen.add(i)
        rec = nodes[i]
        if "capture" in rec:
            out.add(rec["capture"])
        for key in ("child", "parent", "step", "exit"):
            if key in rec:
                stack.append(rec[key])
        stack.extend(rec.get("children", []))
        stack.extend(rec.get("branches", []))
    return frozenset(out)


def _make_anchor_test(anchor: dict):
    """Root test_self from the matcher record's anchor (schema §Matcher record).

    The call_contract check goes through btype (like the Go loader) rather than
    isinstance(CallContractBlock): only CallContractBlock carries that btype,
    and it always exposes `.opcode` as the unsigned 32-bit value.
    """
    if anchor["kind"] == "opcode_set":
        opcodes = frozenset(op & 0xFFFFFFFF for op in anchor["values"])

        def test_opcodes(block: Block) -> bool:
            return block.btype == "call_contract" \
                and getattr(block, "opcode", None) in opcodes

        return test_opcodes
    btypes = frozenset(anchor["values"])

    def test_btypes(block: Block) -> bool:
        return block.btype in btypes

    return test_btypes


class _IrRecursiveMatcher(RecursiveMatcher):
    """RecursiveMatcher that journals per-iteration consumed blocks, mirroring
    the compiler's _MchRecursiveMatcher so list captures inside recursive rule
    bodies can be extracted by the positional walk.

    Recorded per attempt (state reset at every process_child_matcher call, and
    valid because the engine matches sequentially — a probe whose test_self
    fails never reaches process_child_matcher):

    - `_mch_iter_blocks`: each iteration's consumed blocks in iteration order,
      captured BEFORE the engine's set() dedup shuffles the outer list;
    - `_mch_exit_blocks`: the exit branch's consumed blocks (bind-once);
    - `_mch_entry_block`: the entry block, only on success — participation test;
    - `_mch_result_len`: exact length of the last successful try_build result,
      so the walk can skip the recursion's set-shuffled slice precisely.

    Note: the compiler gates the exit probe on `has_pure_exit`; in emitted
    artifacts that is equivalent to `exit` being present (the frontend passes
    exit_matcher=None exactly when there are no pure-exit branches).
    """

    def __init__(self):
        super().__init__(repeating_matcher=None, exit_matcher=None, optional=False)
        self._mch_step_capture_names: frozenset[str] = frozenset()
        self._mch_exit_capture_names: frozenset[str] = frozenset()
        self._mch_iter_blocks: list[list] = []
        self._mch_exit_blocks: list = []
        self._mch_entry_block = None
        self._mch_result_len = 0

    async def try_build(self, block):
        result = await super().try_build(block)
        if result is not None:
            self._mch_result_len = len(result)
        return result

    async def process_child_matcher(self, block, blocks, child_matched):
        self._mch_iter_blocks = []
        self._mch_exit_blocks = []
        self._mch_entry_block = None
        current_blocks = [block]
        blocks_to_add = []
        finished = False
        while True:
            block_for_check = current_blocks[:]
            current_blocks = []
            iter_consumed: list = []
            for c in block_for_check:
                if self.exit_matcher is not None:
                    r = await self.exit_matcher.try_build(c)
                    if r is not None:
                        blocks_to_add.extend(r)
                        self._mch_exit_blocks = list(r)
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
            blocks.extend(set(blocks_to_add))
            child_matched = True
            self._mch_entry_block = block
        return child_matched


class IrRootMatcher(BlockMatcher):
    """A matcher record's anchor node: anchor test as test_self, matcher-level
    include flags, and the build behavior (builder / build program / stub)."""

    def __init__(self, record: dict, registries: Registries):
        super().__init__(
            include_excess=record.get("include_excess", True),
            include_bounces=record.get("include_bounces", True),
        )
        self.name: str = record["name"]
        self.produces: list[str] = list(record["produces"])
        self._registries = registries
        self._anchor_test = _make_anchor_test(record["anchor"])
        self._capture_cards: dict[str, str] = {
            c["name"]: c["card"] for c in record.get("captures", [])
        }
        self._builder_name: str | None = record.get("builder")
        self._program: BuildProgram | None = (
            BuildProgram(record["build_program"], registries)
            if "build_program" in record else None
        )
        self._rejected = False

    def test_self(self, block: Block) -> bool:
        return self._anchor_test(block)

    async def try_build(self, block: Block):
        self._rejected = False
        result = await super().try_build(block)
        if self._rejected:
            # Builder/program rejected: strip the excess/bounce blocks the base
            # try_build auto-appended to our empty return. [] tells callers this
            # was a clean rejection, not a structural mismatch (None).
            return []
        return result

    async def build_block(self, block: Block, other_blocks: list[Block]) -> list[Block]:
        captures = _extract_captures(self, block, other_blocks, self._capture_cards)
        consumed = _dedup_consumed(block, other_blocks)

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
                self._rejected = True
                return []
            produced.merge_blocks([block] + other_blocks)
            return [produced]

        if self._program is not None:
            env = dict(captures)
            # Well-known env names; a capture of the same name shadows them.
            env.setdefault("consumed", consumed)
            env.setdefault("anchor", block)
            try:
                outcome = await self._program.run(env)
            except EvalError as e:
                # Spec §10.7: an expression runtime error is a rejection —
                # never aborts trace processing.
                logger.warning(
                    "mch_ir matcher %r: build evaluation failed (%s); rejecting match",
                    self.name, e,
                )
                outcome = None
            if outcome is None:
                self._rejected = True
                return []
            produced = Block(self.produces[0], [], outcome.data)
            produced.failed = outcome.failed
            produced.broken = outcome.broken
            produced.merge_blocks([block] + other_blocks)
            apply_flows(produced, outcome.flows)
            return [produced]

        # Neither builder nor build program (never emitted by the current
        # frontend): Go-POC stub build — bare primary-btype block over the
        # consumed set.
        produced = Block(self.produces[0], [], None)
        produced.merge_blocks([block] + other_blocks)
        return [produced]


def _dedup_consumed(block: Block, other_blocks: list[Block]) -> list[Block]:
    """The full consumed block set, anchor first, engine traversal order,
    de-duplicated by identity (first occurrence wins — the engine's
    RecursiveMatcher reports a recursion's entry block twice)."""
    consumed: list[Block] = []
    seen_ids: set[int] = set()
    for b in [block] + list(other_blocks):
        if id(b) not in seen_ids:
            seen_ids.add(id(b))
            consumed.append(b)
    return consumed


# ---------------------------------------------------------------------------
# Capture binding: positional walk over the wired node graph against the
# consumed-blocks list. Mirror of compiler._extract_captures/_walk_matcher —
# the engine's try_build accumulates blocks in a fixed traversal order (child
# chain, then children set, then parent chain), and the walk re-traces it.
# ---------------------------------------------------------------------------


def _extract_captures(root: BlockMatcher, anchor_block: Block,
                      other_blocks: list[Block],
                      cards: dict[str, str]) -> dict:
    all_blocks = [anchor_block] + list(other_blocks)
    cursor = [0]
    captures: dict = {
        name: ([] if card == "many" else None) for name, card in cards.items()
    }
    list_names = frozenset(n for n, c in cards.items() if c == "many")
    _walk(root, all_blocks, cursor, captures, list_names, in_recursion=False)
    return captures


def _walk(m: BlockMatcher, all_blocks: list, cursor: list[int],
          captures: dict, list_names: frozenset[str], in_recursion: bool) -> None:
    """Positional walker. `in_recursion=True` for a per-iteration walk inside a
    recursive rule body — the per-iter capture dict is scalar-style (list
    assembly happens in the _IrRecursiveMatcher branch of the outer call)."""
    if cursor[0] >= len(all_blocks):
        return

    if isinstance(m, _IrRecursiveMatcher):
        if in_recursion:
            # Nested recursion: per-iter dicts are scalar-style, list assembly
            # cannot nest. Out of scope; consume the remaining blocks.
            cursor[0] = len(all_blocks)
            return
        # Participation check: the recorded entry block must lie ahead of the
        # cursor in THIS match's consumed list; otherwise the recursion did not
        # take part (optional and absent, or stale state from another probe).
        entry = m._mch_entry_block
        if entry is None or not any(b is entry for b in all_blocks[cursor[0]:]):
            return
        # Per-iteration extraction from the recorded iteration journal. Only
        # captures owned by the STEP subgraph get the one-element-per-unrolling
        # padding (spec §7.2 gap alignment).
        for iter_blocks in m._mch_iter_blocks:
            if not iter_blocks:
                continue
            iter_captures: dict = {name: None for name in captures.keys()}
            _walk(m.repeating_matcher, list(iter_blocks), [0],
                  iter_captures, list_names, in_recursion=True)
            for name in captures.keys():
                if name in list_names and name in m._mch_step_capture_names:
                    captures[name].append(iter_captures.get(name))
        # Exit-branch captures bind at most once — on the terminating match.
        if m.exit_matcher is not None and m._mch_exit_blocks:
            exit_captures: dict = {name: None for name in captures.keys()}
            _walk(m.exit_matcher, list(m._mch_exit_blocks), [0],
                  exit_captures, list_names, in_recursion=True)
            for name in captures.keys():
                if name in list_names and name in m._mch_exit_capture_names:
                    captures[name].append(exit_captures.get(name))
        # Skip exactly the recursion's contribution so captures positioned
        # after it keep binding; if the entry is not at the cursor, fall back
        # to consuming the rest (the compiler's conservative pre-T18 behavior).
        if all_blocks[cursor[0]] is entry and m._mch_result_len > 0:
            cursor[0] += m._mch_result_len
        else:
            cursor[0] = len(all_blocks)
        return

    if isinstance(m, RecursiveMatcher):
        # Non-journaling RecursiveMatcher (defensive — the engine only builds
        # _IrRecursiveMatcher for recursive nodes).
        cursor[0] = len(all_blocks)
        return

    if isinstance(m, OrMatcher):
        block = all_blocks[cursor[0]]
        for sub in m.matchers:
            if sub.test_self(block):
                _walk(sub, all_blocks, cursor, captures, list_names, in_recursion)
                return
        return

    block = all_blocks[cursor[0]]
    if not m.test_self(block):
        # This position was optional and the engine skipped it; cursor stays.
        return

    cap = getattr(m, "_mch_capture_name", None)
    if cap is not None and cap in captures:
        # At the outer walk a list capture is only assembled through the
        # recursion journal above; inside a per-iter walk the dict is
        # scalar-style, so bind unconditionally.
        if in_recursion or cap not in list_names:
            captures[cap] = block
    cursor[0] += 1

    if m.child_matcher is not None:
        _walk(m.child_matcher, all_blocks, cursor, captures, list_names, in_recursion)
    if m.children_matchers:
        for cm in m.children_matchers:
            _walk(cm, all_blocks, cursor, captures, list_names, in_recursion)
    if m.parent_matcher is not None:
        _walk(m.parent_matcher, all_blocks, cursor, captures, list_names, in_recursion)
