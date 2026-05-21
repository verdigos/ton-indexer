from __future__ import annotations

import weakref
from dataclasses import dataclass
from typing import Any

from indexer.events.blocks.basic_matchers import (
    BlockMatcher,
    BlockTypeMatcher,
    ContractMatcher,
    GenericMatcher,
    OrMatcher,
    RecursiveMatcher,
)
from indexer.events.mch import ast_
from indexer.events.mch.diagnostics import CompileError, DiagnosticBag, Span
from indexer.events.mch.recursion import RecursionStrategy, RecursiveMatcherStrategy
from indexer.events.mch.registry import Registries
from indexer.events.mch.resolver import (
    CaptureInfo,
    RecursionClass,
    ResolvedFile,
    ResolvedMatcher,
)


@dataclass
class CompiledMatcher:
    name: str
    matcher: BlockMatcher
    match_cls: type
    build_fn_name: str
    produces_cls_name: str
    source_span: Span


@dataclass
class CompileCtx:
    resolved: ResolvedFile
    registries: Registries
    recursion: RecursionStrategy
    bag: DiagnosticBag
    # Names of recursive rules currently being lowered; used to break cycles
    # by treating self-refs inside step/exit branches as a recursion sentinel.
    lowering_recursive: set[str] = None  # type: ignore[assignment]

    def __post_init__(self) -> None:
        if self.lowering_recursive is None:
            self.lowering_recursive = set()


# Subclass cache: WeakKeyDictionary keyed by Registries -> dict[(name, base) -> subclass].
# Using a weak key lets the cache entry vanish when its Registries is collected, avoiding
# stale closures if a new Registries happens to land at the same address as a dead one.
_GENERATED_SUBCLASSES: "weakref.WeakKeyDictionary[Registries, dict[tuple[str, type], type]]" = (
    weakref.WeakKeyDictionary()
)


class _MchRecursiveMatcher(RecursiveMatcher):
    """RecursiveMatcher specialization that records per-iteration consumed blocks.

    Used by the synthesized walker to populate `list[T]` captures inside
    recursive rule bodies. Each iteration's consumed blocks land in
    `self._mch_iter_blocks` in iteration order BEFORE the engine's `set()`
    dedup step destroys order in the outer `blocks` list.

    Non-reentrant: `_mch_iter_blocks` is reset at the start of every
    `process_child_matcher` call. The engine matches sequentially.
    """

    def __init__(self, repeating_matcher, exit_matcher, optional, has_pure_exit):
        super().__init__(repeating_matcher, exit_matcher, optional)
        self._mch_has_pure_exit = has_pure_exit
        self._mch_iter_blocks: list[list] = []

    async def process_child_matcher(self, block, blocks, child_matched):
        self._mch_iter_blocks = []
        current_blocks = [block]
        blocks_to_add = []
        finished = False
        while True:
            block_for_check = current_blocks[:]
            current_blocks = []
            iter_consumed: list = []
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
            blocks.extend(set(blocks_to_add))
            child_matched = True
        return child_matched


def compile_(
    resolved: ResolvedFile,
    registries: Registries,
    recursion: RecursionStrategy | None = None,
) -> list[CompiledMatcher]:
    """Compile a resolved file to a list of CompiledMatcher.

    Function is named `compile_` (trailing underscore) to avoid shadowing the builtin.
    """
    bag = DiagnosticBag()
    ctx = CompileCtx(
        resolved=resolved,
        registries=registries,
        recursion=recursion or RecursiveMatcherStrategy(),
        bag=bag,
    )

    out: list[CompiledMatcher] = []
    for rm in resolved.matchers:
        out.append(_compile_matcher(rm, ctx))

    if bag.has_errors:
        raise CompileError(bag)

    return out


def _compile_matcher(rm: ResolvedMatcher, ctx: CompileCtx) -> CompiledMatcher:
    decl = rm.decl

    _check_no_capture_after_recursion(decl.pattern, decl.span, ctx)
    if ctx.bag.has_errors:
        raise CompileError(ctx.bag)

    if decl.produces not in ctx.registries.block_types:
        ctx.bag.error(
            "C003_UNKNOWN_BLOCK_TYPE",
            f"matcher {decl.name!r}: produces type {decl.produces!r} is not registered",
            decl.span,
        )
    if decl.build not in ctx.registries.builders:
        ctx.bag.error(
            "C002_UNKNOWN_BUILDER",
            f"matcher {decl.name!r}: builder {decl.build!r} is not registered",
            decl.span,
        )

    # Lower the pattern relative to the anchor.
    parent_m, child_m, children_ms = _lower_relative_to_anchor(decl.pattern, rm.anchor_node, ctx)

    # Build the synthetic subclass based on the anchor head.
    head = rm.anchor_node.head
    if isinstance(head, ast_.OpHead):
        opcode = head.ref if isinstance(head.ref, int) else ctx.resolved.opcodes.get(head.ref) or ctx.registries.opcodes.get(head.ref)
        base = ContractMatcher
        init_kwargs: dict[str, Any] = dict(
            opcode=opcode,
            child_matcher=child_m,
            parent_matcher=parent_m,
            children_matchers=children_ms,
            include_excess=decl.include_excess,
        )
    elif isinstance(head, ast_.BTypeHead):
        base = BlockTypeMatcher
        init_kwargs = dict(
            block_type=head.name,
            child_matcher=child_m,
            parent_matcher=parent_m,
        )
        # BlockTypeMatcher doesn't accept children_matchers in its __init__; if present,
        # wrap by assigning after construction.
    else:
        # Resolver should have rejected this; defensive guard.
        ctx.bag.error(
            "C001_RECURSION_SHAPE_UNSUPPORTED",
            f"matcher {decl.name!r}: unsupported anchor head {type(head).__name__}",
            decl.span,
        )
        raise CompileError(ctx.bag)

    subclass = _make_subclass(decl.name, base, rm, ctx)
    instance: BlockMatcher = subclass(**init_kwargs)
    # Some bases don't accept children_matchers in __init__; apply post-hoc.
    if children_ms is not None and getattr(instance, "children_matchers", None) is None:
        instance.children_matchers = children_ms
    # include_bounces lives on the base; ensure flag is propagated.
    instance.include_bounces = decl.include_bounces
    instance.include_excess = decl.include_excess
    # Apply where-predicate on the anchor node, if any.
    anchor_pred_name = rm.anchor_node.where_predicate
    if anchor_pred_name is not None:
        anchor_pred = ctx.registries.predicates[anchor_pred_name]
        original_test = instance.test_self
        def _wrapped_test_self(block, _orig=original_test, _p=anchor_pred):
            return _orig(block) and _p(block)
        instance.test_self = _wrapped_test_self  # type: ignore[method-assign]
    # Tag the anchor instance with its capture name so the walker can bind it.
    if rm.anchor_capture:
        instance._mch_capture_name = rm.anchor_capture  # type: ignore[attr-defined]

    return CompiledMatcher(
        name=decl.name,
        matcher=instance,
        match_cls=subclass._mch_match_cls,
        build_fn_name=decl.build,
        produces_cls_name=decl.produces,
        source_span=decl.span,
    )


def _make_subclass(name: str, base: type, rm: ResolvedMatcher, ctx: CompileCtx) -> type:
    """Synthesize a BlockMatcher subclass named `_MCH_<name>`.

    The Match dataclass and capture-name set are attached as class attributes
    (`_mch_match_cls`, `_mch_capture_names`) so the synthesized `build_block`
    reads them off `type(self)`. Capture binding is done by `_extract_captures`,
    which walks the matcher tree in the engine's traversal order and pairs
    consumed blocks with the captures their sub-matchers own.

    Cached weakly by Registries: each `Registries` gets its own (name, base) ->
    subclass map. When the Registries is collected, its entries go with it, so
    there's no risk of address reuse handing back a stale closure.
    """
    by_key = _GENERATED_SUBCLASSES.setdefault(ctx.registries, {})
    cache_key = (f"_MCH_{name}", base)
    cached = by_key.get(cache_key)
    if cached is not None:
        return cached

    builder_name = rm.decl.build
    registries = ctx.registries
    match_cls = _make_match_dataclass(
        name,
        rm.captures,
        ctx.registries.block_types,
        opcodes=_merged_opcodes(ctx),
    )
    capture_names = tuple(rm.captures.keys())
    capture_infos = rm.captures  # dict[str, CaptureInfo] — bound in closure scope

    async def build_block(self, block, other_blocks):  # type: ignore[override]
        kwargs = _extract_captures(
            self, block, other_blocks,
            type(self)._mch_capture_names,
            capture_infos,
        )
        m = type(self)._mch_match_cls(**kwargs)
        builder = registries.builders.get(builder_name)
        if builder is None:
            raise RuntimeError(f"builder {builder_name!r} is not registered at run time")
        produced = await builder(m)
        if produced is None:
            self._mch_last_rejected = True
            return []
        self._mch_last_rejected = False
        # Mirror hand-written matchers exactly (e.g. ElectionDepositStakeBlockMatcher
        # at elections.py:46-47): call merge_blocks to wire absorbed blocks'
        # parent linkage, and return [produced] only. The return list advertises the
        # logically-new block; absorption is via the merge_blocks side effect on
        # block.parent. Returning the consumed-blocks list would pollute upstream
        # composers' other_blocks with already-absorbed children.
        produced.merge_blocks([block] + other_blocks)
        return [produced]

    async def try_build(self, block):  # type: ignore[override]
        self._mch_last_rejected = False
        result = await base.try_build(self, block)
        if getattr(self, "_mch_last_rejected", False):
            # Builder rejected: strip any excess/bounce blocks that the base
            # try_build auto-appended to our empty [] return. Return [] so
            # callers checking `not result` see a clean rejection, but callers
            # checking `is not None` still distinguish from a hard structural
            # mismatch (which returns None from the base).
            return []
        return result

    new_cls = type(
        f"_MCH_{name}",
        (base,),
        {
            "__mch_name__": name,
            "__mch_source__": rm.decl.span,
            "_mch_match_cls": match_cls,
            "_mch_capture_names": capture_names,
            "build_block": build_block,
            "try_build": try_build,
        },
    )
    by_key[cache_key] = new_cls
    return new_cls


def _merged_opcodes(ctx: CompileCtx) -> dict[str, int]:
    """File-declared opcodes overlaid on the host registry's opcodes."""
    merged = dict(ctx.registries.opcodes)
    merged.update(ctx.resolved.opcodes)
    return merged


def _make_match_dataclass(
    matcher_name: str,
    captures: dict[str, CaptureInfo],
    block_types: dict[str, type],
    *,
    opcodes: dict[str, int],
) -> type:
    """Build a runtime dataclass with one field per capture."""
    from dataclasses import make_dataclass

    fields: list[tuple[str, Any]] = []
    for cap_name in captures:
        fields.append((cap_name, Any))
    return make_dataclass(f"{matcher_name}Match", fields)


def _extract_captures(
    root: BlockMatcher,
    anchor_block: Any,
    other_blocks: list[Any],
    capture_names: tuple[str, ...],
    capture_infos: dict[str, CaptureInfo],
) -> dict[str, Any]:
    """Walk the lowered matcher tree against the consumed-blocks list and bind captures.

    The engine's `try_build` returns `[anchor_block] + other_blocks`, where the order
    of `other_blocks` follows a fixed traversal of the matcher tree:
    child_matcher's blocks first, then children_matchers in list order, then
    parent_matcher's blocks — each contribution itself recursively expanded.

    This walker mirrors that traversal. list_binding captures (those defined
    inside a recursive rule body) are initialized to [] and populated via
    per-iteration walks against `_MchRecursiveMatcher._mch_iter_blocks`.
    Scalar captures bind via the Phase 1/2 positional walker.
    """
    all_blocks = [anchor_block] + list(other_blocks)
    cursor = [0]
    captures: dict[str, Any] = {
        name: ([] if capture_infos[name].list_binding else None)
        for name in capture_names
    }
    _walk_matcher(root, all_blocks, cursor, captures, capture_infos, in_recursion=False)
    return captures


def _walk_matcher(
    m: BlockMatcher,
    all_blocks: list[Any],
    cursor: list[int],
    captures: dict[str, Any],
    capture_infos: dict[str, CaptureInfo],
    in_recursion: bool,
) -> None:
    """Positional walker. `in_recursion=True` when called for a per-iteration walk
    inside a recursive rule body — disables the list-binding overwrite guard
    because the per-iter capture dict is scalar-style (list assembly happens in
    the outer caller's _MchRecursiveMatcher branch)."""
    if cursor[0] >= len(all_blocks):
        return

    if isinstance(m, _MchRecursiveMatcher):
        # Per-iteration extraction via the matcher's recorded iteration blocks.
        for iter_blocks in m._mch_iter_blocks:
            if not iter_blocks:
                continue
            iter_cursor = [0]
            iter_captures: dict[str, Any] = {name: None for name in captures.keys()}
            _walk_matcher(
                m.repeating_matcher, list(iter_blocks), iter_cursor,
                iter_captures, capture_infos, in_recursion=True,
            )
            for name, info in capture_infos.items():
                if info.list_binding:
                    captures[name].append(iter_captures.get(name))
        # set() in engine destroys positional alignment for outside-recursion
        # captures appearing AFTER the recursion (out of scope, see spec §6).
        cursor[0] = len(all_blocks)
        return

    if isinstance(m, RecursiveMatcher):
        # Non-_Mch RecursiveMatcher (defensive — shouldn't occur post-Phase 3).
        cursor[0] = len(all_blocks)
        return

    if isinstance(m, OrMatcher):
        block = all_blocks[cursor[0]]
        for sub in m.matchers:
            if sub.test_self(block):
                _walk_matcher(sub, all_blocks, cursor, captures, capture_infos, in_recursion)
                return
        return

    block = all_blocks[cursor[0]]
    if not m.test_self(block):
        # This position was optional and the engine skipped it; cursor stays.
        return

    cap = getattr(m, "_mch_capture_name", None)
    if cap is not None and cap in captures:
        info = capture_infos.get(cap)
        # CORRECTNESS GUARD: at the OUTER walk (in_recursion=False), do NOT
        # overwrite a list_binding capture — the list is populated only via
        # the _MchRecursiveMatcher branch above. Inside a per-iter walk
        # (in_recursion=True), the iter_captures dict is scalar-style; bind
        # unconditionally so per-iter values get collected.
        if in_recursion or (info is None or not info.list_binding):
            captures[cap] = block
    cursor[0] += 1

    if m.child_matcher is not None:
        _walk_matcher(m.child_matcher, all_blocks, cursor, captures, capture_infos, in_recursion)
    if m.children_matchers:
        for cm in m.children_matchers:
            _walk_matcher(cm, all_blocks, cursor, captures, capture_infos, in_recursion)
    if m.parent_matcher is not None:
        _walk_matcher(m.parent_matcher, all_blocks, cursor, captures, capture_infos, in_recursion)


def _lower_relative_to_anchor(
    pattern: ast_.PatternExpr,
    anchor: ast_.Node,
    ctx: CompileCtx,
) -> tuple[BlockMatcher | None, BlockMatcher | None, list[BlockMatcher] | None]:
    """Walk the pattern to figure out (parent_matcher, child_matcher, children_matchers)."""
    parents = _find_anchor_path(pattern, anchor)
    if parents is None:
        return None, None, None
    # `parents` ends with the anchor itself; its immediate parent is parents[-2].
    if len(parents) < 2:
        return None, None, None
    immediate_parent = parents[-2]

    child_chain, children_ms = _lower_child_chain_from(immediate_parent, pattern, anchor, ctx)
    parent_chain = _lower_parent_chain_from(immediate_parent, pattern, anchor, ctx)
    return parent_chain, child_chain, children_ms


def _find_anchor_path(pattern: ast_.PatternExpr, anchor: ast_.Node) -> list[ast_.PatternExpr] | None:
    if pattern is anchor:
        return None

    def search(p: ast_.PatternExpr, path: list[ast_.PatternExpr]) -> list[ast_.PatternExpr] | None:
        if p is anchor:
            return path + [p]
        if isinstance(p, ast_.Sequence):
            r = search(p.head, path + [p])
            if r:
                return r
            for _e, atom in p.tail:
                r = search(atom, path + [p])
                if r:
                    return r
        elif isinstance(p, ast_.Maybe):
            return search(p.inner, path + [p])
        elif isinstance(p, ast_.Alternative):
            for b in p.branches:
                r = search(b, path + [p])
                if r:
                    return r
        elif isinstance(p, ast_.ChildrenBlock):
            for it in p.items:
                r = search(it, path + [p])
                if r:
                    return r
        return None

    return search(pattern, [])


def _lower_child_chain_from(
    parent: ast_.PatternExpr,
    pattern: ast_.PatternExpr,
    anchor: ast_.Node,
    ctx: CompileCtx,
) -> tuple[BlockMatcher | None, list[BlockMatcher] | None]:
    if isinstance(parent, ast_.Sequence):
        downstream_atoms: list[ast_.PatternExpr] = []
        if parent.head is anchor:
            downstream_atoms = [atom for edge, atom in parent.tail
                                if edge is ast_.Edge.CHILD]
        else:
            anchor_idx = None
            for i, (_e, atom) in enumerate(parent.tail):
                if atom is anchor:
                    anchor_idx = i
                    break
            if anchor_idx is not None:
                downstream_atoms = [atom for edge, atom in parent.tail[anchor_idx + 1:]
                                    if edge is ast_.Edge.CHILD]
        if not downstream_atoms:
            return None, None
        first = downstream_atoms[0]
        if isinstance(first, ast_.ChildrenBlock):
            children_ms = [_lower_atom(it, ctx) for it in first.items]
            rest = downstream_atoms[1:]
            if rest:
                child = _chain(_lower_atom(a, ctx) for a in rest)
                return child, children_ms
            return None, children_ms
        child = _chain(_lower_atom(a, ctx) for a in downstream_atoms)
        return child, None

    return None, None


def _lower_parent_chain_from(
    parent: ast_.PatternExpr,
    pattern: ast_.PatternExpr,
    anchor: ast_.Node,
    ctx: CompileCtx,
) -> BlockMatcher | None:
    """Lower consecutive PARENT edges from the anchor outward into a chain of
    parent_matchers. Handles anchor-is-head and both anchor-in-tail variants.
    """
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
            # Variant B.1: anchor's incoming edge is PARENT; the "previous" atom
            # acts as the first parent link.
            prepend_previous = True
            start_idx = anchor_idx + 1
        else:
            # Variant B.2: anchor's incoming edge is CHILD; walk forward through
            # consecutive PARENT edges.
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


def _lower_atom(p: ast_.PatternExpr, ctx: CompileCtx) -> BlockMatcher:
    if isinstance(p, ast_.Node):
        return _lower_node(p, ctx)
    if isinstance(p, ast_.Maybe):
        inner = _lower_atom(p.inner, ctx)
        inner.optional = True
        return inner
    if isinstance(p, ast_.Alternative):
        return OrMatcher([_lower_atom(b, ctx) for b in p.branches])
    if isinstance(p, ast_.Sequence):
        first = _lower_atom(p.head, ctx)
        for _edge, atom in p.tail:
            inner = _lower_atom(atom, ctx)
            _attach_as_child(first, inner)
        return first
    if isinstance(p, ast_.ChildrenBlock):
        return OrMatcher([_lower_atom(it, ctx) for it in p.items])
    if isinstance(p, ast_.RuleRef):
        target = next((r for r in ctx.resolved.raw.rules if r.name == p.name), None)
        if target is None:
            raise CompileError(ctx.bag)
        # Self-reference inside the body of a recursive rule we're already lowering:
        # the RecursiveMatcher handles the repetition, so we represent the self-ref
        # as a permissive sentinel.
        if p.name in ctx.lowering_recursive:
            return GenericMatcher(test_self_func=lambda _b: True)
        rec_class = ctx.resolved.rule_recursion.get(p.name)
        if rec_class is RecursionClass.INDIRECT_RECURSIVE:
            ctx.bag.error(
                "C005_INDIRECT_RECURSION_UNSUPPORTED",
                f"rule {p.name!r}: indirect (mutual) recursion is not supported in v0; "
                f"flatten the rule cycle or use a single self-referential rule",
                p.span,
            )
            raise CompileError(ctx.bag)
        if rec_class is RecursionClass.DIRECT_RECURSIVE:
            return _lower_recursive_rule(p.name, ctx)
        return _lower_atom(target.pattern, ctx)
    raise AssertionError(f"unhandled pattern node: {type(p).__name__}")


def _lower_node(n: ast_.Node, ctx: CompileCtx) -> BlockMatcher:
    head = n.head
    if isinstance(head, ast_.OpHead):
        opcode = head.ref if isinstance(head.ref, int) else (
            ctx.resolved.opcodes.get(head.ref) or ctx.registries.opcodes.get(head.ref)
        )
        m: BlockMatcher = ContractMatcher(opcode=opcode)
    elif isinstance(head, ast_.BTypeHead):
        m = BlockTypeMatcher(block_type=head.name)
    elif isinstance(head, ast_.PredHead):
        pred = ctx.registries.predicates[head.name]
        m = GenericMatcher(test_self_func=pred)
    elif isinstance(head, ast_.AnyHead):
        m = GenericMatcher(test_self_func=lambda _b: True)
    else:
        raise AssertionError(f"unknown node head: {type(head).__name__}")
    if n.where_predicate is not None:
        pred = ctx.registries.predicates[n.where_predicate]
        original_test = m.test_self
        def combined(block, _orig=original_test, _p=pred):
            return _orig(block) and _p(block)
        m.test_self = combined  # type: ignore[assignment]
    if n.capture is not None:
        # Tag the lowered matcher with its capture name. The synthesized
        # build_block walks the matcher tree and binds captures positionally
        # by consulting this attribute.
        m._mch_capture_name = n.capture  # type: ignore[attr-defined]
    return m


def _chain(matchers_iter) -> BlockMatcher | None:
    matchers = [m for m in matchers_iter if m is not None]
    if not matchers:
        return None
    first = matchers[0]
    current = first
    for nxt in matchers[1:]:
        _attach_as_child(current, nxt)
        current = nxt
    return first


def _attach_as_child(host: BlockMatcher, child: BlockMatcher) -> None:
    if host.child_matcher is None:
        host.child_matcher = child
    else:
        _attach_as_child(host.child_matcher, child)


def _attach_as_parent(host: BlockMatcher, parent: BlockMatcher) -> None:
    if host.parent_matcher is None:
        host.parent_matcher = parent
    else:
        _attach_as_parent(host.parent_matcher, parent)


def _check_no_capture_after_recursion(
    pattern: ast_.PatternExpr,
    decl_span: Span,
    ctx: CompileCtx,
) -> None:
    """Reject captures placed AFTER a direct-recursive rule reference in the
    same sequence — the walker cannot reach them due to engine `set()` dedup."""

    def is_direct_recursive_ref(p: ast_.PatternExpr) -> bool:
        if isinstance(p, ast_.RuleRef):
            return ctx.resolved.rule_recursion.get(p.name) is RecursionClass.DIRECT_RECURSIVE
        if isinstance(p, ast_.Maybe):
            return is_direct_recursive_ref(p.inner)
        return False

    def contains_capture(p: ast_.PatternExpr) -> bool:
        if isinstance(p, ast_.Node):
            return p.capture is not None
        if isinstance(p, ast_.Maybe):
            return contains_capture(p.inner)
        if isinstance(p, ast_.Sequence):
            if contains_capture(p.head):
                return True
            return any(contains_capture(a) for _e, a in p.tail)
        if isinstance(p, ast_.Alternative):
            return any(contains_capture(b) for b in p.branches)
        if isinstance(p, ast_.ChildrenBlock):
            return any(contains_capture(it) for it in p.items)
        if isinstance(p, ast_.RuleRef):
            target = next((r for r in ctx.resolved.raw.rules if r.name == p.name), None)
            return target is not None and contains_capture(target.pattern)
        return False

    def walk(p: ast_.PatternExpr) -> None:
        if isinstance(p, ast_.Sequence):
            recursion_seen = False
            atoms = [(None, p.head)] + list(p.tail)
            for _edge, atom in atoms:
                if recursion_seen and contains_capture(atom):
                    ctx.bag.error(
                        "C007_CAPTURE_AFTER_RECURSION",
                        "captures placed after a recursive-rule reference in the "
                        "same sequence are not bound at runtime; restructure the pattern",
                        atom.span if hasattr(atom, "span") else decl_span,
                    )
                if is_direct_recursive_ref(atom):
                    recursion_seen = True
                walk(atom)
        elif isinstance(p, ast_.Maybe):
            walk(p.inner)
        elif isinstance(p, ast_.Alternative):
            for b in p.branches:
                walk(b)
        elif isinstance(p, ast_.ChildrenBlock):
            for it in p.items:
                walk(it)

    walk(pattern)


def _strip_trailing_self_ref(branch: ast_.PatternExpr, rule_name: str) -> ast_.PatternExpr | None:
    """Strip trailing `maybe $rule_name` from a step branch.

    Returns the stripped pattern, or None if the branch is just `maybe $rule_name`
    alone (the branch carries no actual content and should be discarded).
    """
    if isinstance(branch, ast_.Maybe) and isinstance(branch.inner, ast_.RuleRef) and branch.inner.name == rule_name:
        return None
    if isinstance(branch, ast_.Sequence):
        if branch.tail:
            last_edge, last_atom = branch.tail[-1]
            if (
                isinstance(last_atom, ast_.Maybe)
                and isinstance(last_atom.inner, ast_.RuleRef)
                and last_atom.inner.name == rule_name
            ):
                new_tail = branch.tail[:-1]
                if not new_tail:
                    return branch.head
                return ast_.Sequence(head=branch.head, tail=new_tail, span=branch.span)
    return branch


def _lower_recursive_rule(name: str, ctx: CompileCtx) -> BlockMatcher:
    """Lower a recursive rule via the configured RecursionStrategy.

    Phase 3 change: strip trailing `maybe $self` from step branches; pass
    `exit_matcher=None` when there are no pure-exit branches (to avoid the v0
    'always-one-iteration' bug where an `exit_matcher=always_true` short-circuits
    the loop). Use `_MchRecursiveMatcher` to record per-iteration consumed blocks
    for list-capture extraction.
    """
    target = next(r for r in ctx.resolved.raw.rules if r.name == name)
    pattern = target.pattern

    step_branches, exit_branches = _split_recursion_branches(pattern, name)
    has_pure_exit = bool(exit_branches)

    # Strip trailing self-refs from step branches.
    stripped_step_branches: list[ast_.PatternExpr] = []
    for b in step_branches:
        s = _strip_trailing_self_ref(b, name)
        if s is not None:
            stripped_step_branches.append(s)

    if not stripped_step_branches:
        # Fallback: stripping discarded every step branch (e.g. a rule shape like
        # `op A -> ( maybe $self | op B )` where _split_recursion_branches sees
        # only the Alternative branches and the `maybe $self` is the entire step).
        # Preserve v0 behavior by keeping the original branches; the permissive
        # sentinel will fire on the self-ref. The recursion will be bounded by
        # the engine's per-iteration block exhaustion rather than the strip.
        if not step_branches:
            ctx.bag.error(
                "C001_RECURSION_SHAPE_UNSUPPORTED",
                f"rule {name!r}: no non-recursive step branch found",
                target.span,
            )
            raise CompileError(ctx.bag)
        stripped_step_branches = list(step_branches)

    ctx.lowering_recursive.add(name)
    try:
        if len(stripped_step_branches) == 1:
            step = _lower_atom(stripped_step_branches[0], ctx)
        else:
            step = OrMatcher([_lower_atom(b, ctx) for b in stripped_step_branches])

        if has_pure_exit:
            if len(exit_branches) == 1:
                exit_m: BlockMatcher | None = _lower_atom(exit_branches[0], ctx)
            else:
                exit_m = OrMatcher([_lower_atom(b, ctx) for b in exit_branches])
        else:
            exit_m = None
    finally:
        ctx.lowering_recursive.discard(name)

    return _MchRecursiveMatcher(
        repeating_matcher=step,
        exit_matcher=exit_m,
        optional=True,
        has_pure_exit=has_pure_exit,
    )


def _split_recursion_branches(
    pattern: ast_.PatternExpr,
    rule_name: str,
) -> tuple[list[ast_.PatternExpr], list[ast_.PatternExpr]]:
    """Heuristic: separate (step, exit) branches.

    - Branches that *do not* reference `$rule_name` are exits.
    - Branches that reference `$rule_name` outside `maybe` are steps.
    - Branches whose only $rule_name reference is inside `maybe` are also steps
      (the maybe is the termination marker).

    Returns (step_branches, exit_branches). For non-Alternative patterns we return
    one step branch (the whole pattern) and no exits.
    """
    if isinstance(pattern, ast_.Alternative):
        steps: list[ast_.PatternExpr] = []
        exits: list[ast_.PatternExpr] = []
        for b in pattern.branches:
            if _contains_rule_ref(b, rule_name):
                steps.append(b)
            else:
                exits.append(b)
        return steps, exits
    # Sequence with an alternative inside its tail: search recursively.
    if isinstance(pattern, ast_.Sequence):
        # Look for an Alternative within the tail; if present, split it.
        for _e, atom in pattern.tail:
            if isinstance(atom, ast_.Alternative):
                return _split_recursion_branches(atom, rule_name)
    return [pattern], []


def _contains_rule_ref(p: ast_.PatternExpr, name: str) -> bool:
    if isinstance(p, ast_.RuleRef):
        return p.name == name
    if isinstance(p, ast_.Maybe):
        return _contains_rule_ref(p.inner, name)
    if isinstance(p, ast_.Sequence):
        if _contains_rule_ref(p.head, name):
            return True
        return any(_contains_rule_ref(a, name) for _e, a in p.tail)
    if isinstance(p, ast_.Alternative):
        return any(_contains_rule_ref(b, name) for b in p.branches)
    if isinstance(p, ast_.ChildrenBlock):
        return any(_contains_rule_ref(it, name) for it in p.items)
    return False
