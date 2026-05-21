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

    _check_no_multi_hop_parent(decl.pattern, decl.span, ctx)

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

    async def build_block(self, block, other_blocks):  # type: ignore[override]
        kwargs = _extract_captures(self, block, other_blocks, type(self)._mch_capture_names)
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
) -> dict[str, Any]:
    """Walk the lowered matcher tree against the consumed-blocks list and bind captures.

    The engine's `try_build` returns `[anchor_block] + other_blocks`, where the order
    of `other_blocks` follows a fixed traversal of the matcher tree:
    child_matcher's blocks first, then children_matchers in list order, then
    parent_matcher's blocks — each contribution itself recursively expanded.

    This walker mirrors that traversal. For each sub-matcher visited, it:
      - checks `test_self` against the cursor block;
      - if the matcher owns a capture (tagged at lowering time as
        `_mch_capture_name`), binds the cursor block to that capture;
      - advances the cursor and recurses into the sub-matcher's own children.

    Captures whose owning sub-matcher is never reached (e.g. an optional branch
    that didn't fire, or a position inside a RecursiveMatcher whose contents
    are opaque in v0) stay None. List bindings inside recursive rules are
    Phase 3's responsibility; here they remain None.
    """
    all_blocks = [anchor_block] + list(other_blocks)
    cursor = [0]
    captures: dict[str, Any] = {name: None for name in capture_names}
    _walk_matcher(root, all_blocks, cursor, captures)
    return captures


def _walk_matcher(
    m: BlockMatcher,
    all_blocks: list[Any],
    cursor: list[int],
    captures: dict[str, Any],
) -> None:
    if cursor[0] >= len(all_blocks):
        return

    # RecursiveMatcher is opaque in Phase 1: its iterations may add blocks in
    # non-deterministic order (the engine uses `set()` to dedupe), so we can't
    # reliably map positions to captures here. Consume the rest of the buffer
    # so any siblings appearing after the recursion don't bind erroneously.
    if isinstance(m, RecursiveMatcher):
        cursor[0] = len(all_blocks)
        return

    if isinstance(m, OrMatcher):
        block = all_blocks[cursor[0]]
        for sub in m.matchers:
            if sub.test_self(block):
                _walk_matcher(sub, all_blocks, cursor, captures)
                return
        return

    block = all_blocks[cursor[0]]
    if not m.test_self(block):
        # This position was optional and the engine skipped it; cursor stays.
        return

    cap = getattr(m, "_mch_capture_name", None)
    if cap is not None and cap in captures:
        captures[cap] = block
    cursor[0] += 1

    if m.child_matcher is not None:
        _walk_matcher(m.child_matcher, all_blocks, cursor, captures)
    if m.children_matchers:
        for cm in m.children_matchers:
            _walk_matcher(cm, all_blocks, cursor, captures)
    if m.parent_matcher is not None:
        _walk_matcher(m.parent_matcher, all_blocks, cursor, captures)


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
    if isinstance(parent, ast_.Sequence):
        if parent.head is anchor:
            # Anchor is the head; if the first tail edge is PARENT, that atom is
            # the parent of the anchor (e.g. `op A @a <- op B @b`).
            if parent.tail and parent.tail[0][0] is ast_.Edge.PARENT:
                return _lower_atom(parent.tail[0][1], ctx)
            return None
        for i, (edge, atom) in enumerate(parent.tail):
            if atom is anchor and edge is ast_.Edge.PARENT:
                previous = parent.head if i == 0 else parent.tail[i - 1][1]
                return _lower_atom(previous, ctx)
    return None


def _check_no_multi_hop_parent(pattern: ast_.PatternExpr, decl_span: Span, ctx: CompileCtx) -> None:
    """Reject patterns with two or more consecutive PARENT edges.

    v0 only supports a single PARENT edge per position; multi-hop chains
    (`X <- Y <- Z`) would be silently truncated by the lowering, so we surface
    them as C006 instead.
    """
    def walk(p: ast_.PatternExpr) -> None:
        if isinstance(p, ast_.Sequence):
            prev_was_parent = False
            for edge, atom in p.tail:
                if edge is ast_.Edge.PARENT and prev_was_parent:
                    ctx.bag.error(
                        "C006_MULTI_HOP_PARENT_UNSUPPORTED",
                        "multi-hop parent chain (consecutive `<-` edges) is not supported in v0",
                        decl_span,
                    )
                    raise CompileError(ctx.bag)
                prev_was_parent = (edge is ast_.Edge.PARENT)
                walk(atom)
            walk(p.head)
        elif isinstance(p, ast_.Maybe):
            walk(p.inner)
        elif isinstance(p, ast_.Alternative):
            for b in p.branches:
                walk(b)
        elif isinstance(p, ast_.ChildrenBlock):
            for it in p.items:
                walk(it)

    walk(pattern)


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


def _lower_recursive_rule(name: str, ctx: CompileCtx) -> BlockMatcher:
    """Lower a recursive rule via the configured RecursionStrategy.

    v0 strategy: decompose the rule body into a "step" (non-recursive branches)
    and an "exit" (the branch(es) reachable when the recursion terminates) and
    emit RecursiveMatcher(step, exit).
    """
    target = next(r for r in ctx.resolved.raw.rules if r.name == name)
    pattern = target.pattern

    step_branches, exit_branches = _split_recursion_branches(pattern, name)

    # Mark this rule as being lowered so that self-refs inside step/exit branches
    # are treated as a permissive sentinel rather than triggering infinite recursion.
    ctx.lowering_recursive.add(name)
    try:
        # Lower step.
        if not step_branches:
            ctx.bag.error(
                "C001_RECURSION_SHAPE_UNSUPPORTED",
                f"rule {name!r}: no non-recursive step branch found",
                target.span,
            )
            raise CompileError(ctx.bag)
        if len(step_branches) == 1:
            step = _lower_atom(step_branches[0], ctx)
        else:
            step = OrMatcher([_lower_atom(b, ctx) for b in step_branches])

        # Lower exit. If no exit branches were found (rule body only had recursive refs),
        # use a permissive matcher.
        if not exit_branches:
            exit_m: BlockMatcher = GenericMatcher(test_self_func=lambda _b: True)
        elif len(exit_branches) == 1:
            exit_m = _lower_atom(exit_branches[0], ctx)
        else:
            exit_m = OrMatcher([_lower_atom(b, ctx) for b in exit_branches])
    finally:
        ctx.lowering_recursive.discard(name)

    return RecursiveMatcher(repeating_matcher=step, exit_matcher=exit_m, optional=True)


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
