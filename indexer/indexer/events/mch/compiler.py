from __future__ import annotations

from dataclasses import dataclass, replace
from typing import Any

from indexer.events.mch import ast_
from indexer.events.mch.builtin_signatures import BUILTIN_SIGNATURES
from indexer.events.mch.diagnostics import CompileError, DiagnosticBag, Span
from indexer.events.mch.registry import Registries
from indexer.events.mch.resolver import (
    CaptureInfo,
    RecursionClass,
    ResolvedFile,
    ResolvedMatcher,
    _alternative_is_exclusive,
    _resolve_opcode_ref,
)


@dataclass
class LNode:
    """A lowered IR node.

    The frontend lowers `.mch` patterns straight into a graph of these plain
    records. No engine primitives are constructed. `ir_emit` walks the graph in
    DFS pre-order to assign node ids and produce the JSON artifact; the v2 IR
    engine (`mch_ir/`) is the only runtime that consumes it.

    `kind` ∈ {"contract", "block_type", "pred", "any", "or", "recursive",
    "cyclic_ref"}. Field usage by kind:
      - contract:   `opcode`
      - block_type: `btype`
      - pred:       `pred`
      - any:        (leaf, permissive; anchor-group / predicate-anchor roots,
                    recursion sentinels, `any` heads)
      - or:         `branches`, `exclusive`
      - recursive:  `step`, `exit_` (frontier), or `strategy="cyclic"` + `step`
      - cyclic_ref: `target`, an in-body self-reference back-edge; carries no
                    node of its own (emitter resolves it to `target`'s id).
    Common optional fields on every kind: `optional`, `peek`, `capture`, `where`,
    `where_expr`, `child`, `children`, `parent`.
    """

    kind: str
    opcode: int | None = None
    btype: str | None = None
    pred: str | None = None
    branches: list["LNode"] | None = None
    exclusive: bool = False
    strategy: str | None = None            # "cyclic" or None (frontier)
    step: "LNode | None" = None
    exit_: "LNode | None" = None
    target: "LNode | None" = None          # cyclic_ref back-edge target
    optional: bool = False
    peek: bool = False
    many: bool = False
    max: int | None = None
    peek_span: Span | None = None
    capture: str | None = None
    where: str | None = None
    where_expr: "ast_.Expr | None" = None
    child: "LNode | None" = None
    children: list["LNode"] | None = None
    parent: "LNode | None" = None


@dataclass
class CompiledMatcher:
    name: str
    root: LNode
    source_span: Span
    produces: tuple[str, ...] = ()
    captures: tuple[tuple[str, str], ...] = ()
    include_excess: bool = True
    include_bounces: bool = True
    build_stmts: tuple = ()
    shape_fn_name: str | None = None
    # Anchor record representation. Exactly one of
    # these is set for a group/predicate anchor; a plain single op/btype anchor
    # leaves them all None and the emitter reads the root node's kind.
    anchor_pred: str | None = None
    anchor_branches: tuple | None = None
    anchor_opcodes: tuple[int, ...] | None = None
    anchor_btypes: tuple[str, ...] | None = None


@dataclass
class CompileCtx:
    resolved: ResolvedFile
    registries: Registries
    bag: DiagnosticBag
    where_anchor_capture: str = ""
    # Names of recursive rules currently being lowered; used to break cycles
    # by treating self-refs inside step/exit branches as a recursion sentinel.
    lowering_recursive: set[str] = None  # type: ignore[assignment]

    def __post_init__(self) -> None:
        if self.lowering_recursive is None:
            self.lowering_recursive = set()


def compile_(
    resolved: ResolvedFile,
    registries: Registries,
) -> list[CompiledMatcher]:
    """Compile a resolved file to a list of CompiledMatcher.

    Function is named `compile_` (trailing underscore) to avoid shadowing the builtin.
    """
    bag = DiagnosticBag()
    ctx = CompileCtx(
        resolved=resolved,
        registries=registries,
        bag=bag,
    )

    out: list[CompiledMatcher] = []
    for rm in resolved.matchers:
        out.append(_compile_matcher(rm, ctx))

    if bag.items:
        raise CompileError(bag)

    return out


def _compile_matcher(rm: ResolvedMatcher, ctx: CompileCtx) -> CompiledMatcher:
    decl = rm.decl
    ctx.where_anchor_capture = rm.anchor_capture

    _check_no_capture_after_recursion(decl.pattern, decl.span, ctx)
    _check_children_block_placement(decl.pattern, ctx)
    if ctx.bag.items:
        raise CompileError(ctx.bag)

    parent_m, child_m, children_ms = _lower_surrounding_sequence(decl.pattern, ctx)

    head = rm.anchor_node.head
    anchor_pred: str | None = None
    anchor_branches: tuple | None = None
    anchor_opcodes: tuple[int, ...] | None = None
    anchor_btypes: tuple[str, ...] | None = None
    if rm.anchor_group is not None:
        # Membership is recorded separately; the graph root remains permissive.
        if rm.anchor_group.exclusive:
            _check_redundant_exclusive_heads(rm.anchor_group, ctx)
        opcode_set, btype_set = _anchor_group_head_sets(rm.anchor_group, ctx)
        branch_descs = _anchor_group_branch_descs(rm.anchor_group, ctx)
        any_where = any(w is not None for _k, _v, w in branch_descs)
        root = LNode(kind="any", child=child_m, parent=parent_m)
    elif isinstance(head, ast_.OpHead):
        opcode = _resolve_opcode_ref(head.ref, ctx.resolved.opcodes, ctx.registries)
        root = LNode(
            kind="contract",
            opcode=opcode,
            child=child_m,
            parent=parent_m,
            children=children_ms,
        )
    elif isinstance(head, ast_.BTypeHead):
        root = LNode(kind="block_type", btype=head.name, child=child_m, parent=parent_m)
    else:
        # Predicate anchors have no opcode/btype prefilter and are full-scanned.
        anchor_pred = head.name
        root = LNode(kind="any", child=child_m, parent=parent_m)

    # Some heads carry children_matchers only post-hoc (only `contract` takes
    # them at construction); apply here if not already set.
    if children_ms is not None and root.children is None:
        root.children = children_ms
    if rm.anchor_node.relations is not None:
        _lower_relations(rm.anchor_node.relations, root, ctx)
    # Apply where-predicate on the anchor node, if any. For a set-membership
    # group anchor, the per-branch `where` is already folded into
    # the group's membership record; `anchor_node` is only a representative
    # branch, so its `where` is not re-applied to the root node.
    anchor_pred_name = None if rm.anchor_group is not None else rm.anchor_node.where_predicate
    if anchor_pred_name is not None:
        root.where = anchor_pred_name
    if rm.anchor_capture:
        root.capture = rm.anchor_capture
    if rm.anchor_group is None and rm.anchor_node.where_expr is not None:
        _set_where_expr(root, rm.anchor_node.where_expr, rm.anchor_node.span, ctx)
    if rm.anchor_group is not None:
        # Group-anchor branches carry only named `where` predicates in the IR.
        # Reject an inline expression rather than dropping it.
        for b in rm.anchor_group.branches:
            if isinstance(b, ast_.Node) and b.where_expr is not None:
                ctx.bag.error(
                    "C013_WHERE_EXPR_NOT_SYNC",
                    f"matcher {decl.name!r}: inline `where (expr)` is not "
                    f"supported on opcode-set anchor branches; use a named "
                    f"`where <predicate>`",
                    b.span,
                )
    if rm.anchor_group is not None:
        # A pure single-kind group with no branch `where` keeps the
        # `opcode_set`/`btype` forms.
        # a mixed-kind group or any branch `where` emits the `mixed` branch form
        # (see ir_emit._anchor_record). Record exactly one representation.
        kinds = {k for k, _v, _w in branch_descs}
        if len(kinds) == 1 and not any_where:
            if kinds == {"op"}:
                anchor_opcodes = tuple(sorted(opcode_set))
            else:
                anchor_btypes = tuple(sorted(btype_set))
        else:
            anchor_branches = tuple(branch_descs)

    _check_peek_subtree_closure(root, decl.name, ctx)

    return CompiledMatcher(
        name=decl.name,
        root=root,
        source_span=decl.span,
        produces=tuple(decl.produces),
        captures=tuple(
            (cname, "many" if ci.list_binding else "one")
            for cname, ci in rm.captures.items()
        ),
        include_excess=bool(decl.include_excess),
        include_bounces=bool(decl.include_bounces),
        build_stmts=tuple(decl.build_stmts),
        shape_fn_name=decl.shape,
        anchor_pred=anchor_pred,
        anchor_branches=anchor_branches,
        anchor_opcodes=anchor_opcodes,
        anchor_btypes=anchor_btypes,
    )


def _lower_surrounding_sequence(
    pattern: ast_.PatternExpr,
    ctx: CompileCtx,
) -> tuple[LNode | None, LNode | None, list[LNode] | None]:
    if not isinstance(pattern, ast_.Sequence):
        return None, None, None
    child_chain, children_ms = _lower_child_chain_from(pattern, ctx)
    parent_chain = _lower_parent_chain_from(pattern, ctx)
    return parent_chain, child_chain, children_ms


def _is_cb_atom(a: ast_.PatternExpr) -> bool:
    """Whether an atom attaches left without advancing the chain cursor.
    """
    return isinstance(a, ast_.ChildrenBlock)


def _lower_child_chain_from(
    sequence: ast_.Sequence,
    ctx: CompileCtx,
) -> tuple[LNode | None, list[LNode] | None]:
    downstream_atoms: list[ast_.PatternExpr] = []
    for edge, atom in sequence.tail:
        if edge is ast_.Edge.PARENT:
            if downstream_atoms:
                ctx.bag.error(
                    "C009_MIXED_CHILD_PARENT_CHAIN_UNSUPPORTED",
                    "a parent edge ('<-') following a child edge in the same "
                    "chain is unsupported: the parent would test the anchor "
                    "block and can never match; split the pattern",
                    atom.span,
                )
                raise CompileError(ctx.bag)
            break
        downstream_atoms.append(atom)
    if not downstream_atoms:
        return None, None
    first = downstream_atoms[0]
    if isinstance(first, ast_.ChildrenBlock):
        return None, [_lower_atom(it, ctx) for it in first.items]
    pairs = [(ast_.Edge.CHILD, a) for a in downstream_atoms]
    child = _lower_chain_with_children_attach(pairs, ctx)
    return child, None


def _lower_parent_chain_from(
    sequence: ast_.Sequence,
    ctx: CompileCtx,
) -> LNode | None:
    idx = 0
    if idx >= len(sequence.tail) or sequence.tail[idx][0] is not ast_.Edge.PARENT:
        return None

    # Parent edges extend the chain. Children blocks attach to the current tail
    # without advancing it; other child branches must be grouped with the
    # parent target.
    chain_pairs: list[tuple[ast_.Edge, ast_.PatternExpr]] = []
    for edge, atom in sequence.tail[idx:]:
        if edge is ast_.Edge.PARENT:
            chain_pairs.append((ast_.Edge.PARENT, atom))
        elif _is_cb_atom(atom):
            chain_pairs.append((ast_.Edge.CHILD, atom))
        else:
            ctx.bag.error(
                "C009_MIXED_CHILD_PARENT_CHAIN_UNSUPPORTED",
                "a child branch after a parent link must be grouped with its "
                "parent target, e.g. `op A @a <- (op P -> op S)`",
                atom.span,
            )
            raise CompileError(ctx.bag)

    return _lower_chain_with_children_attach(chain_pairs, ctx)


def _lower_chain_with_children_attach(
    pairs: list[tuple[ast_.Edge, ast_.PatternExpr]],
    ctx: CompileCtx,
) -> LNode | None:
    """Walk an ordered list of `(edge, atom)` pairs and build a chained node graph.

    ChildrenBlock atoms attach their items to the immediately-preceding node's
    `children`.
    Bare ChildrenBlock at the head (no left sibling) emits C008. Non-CB atoms
    chain via child or parent edges.
    """
    head_matcher: LNode | None = None
    previous_lowered: LNode | None = None

    for edge, atom in pairs:
        if isinstance(atom, ast_.ChildrenBlock):
            if previous_lowered is None:
                # Bare ChildrenBlock with no left sibling.
                ctx.bag.error(
                    "C008_CHILDREN_BLOCK_BARE",
                    "children block '{...}' must follow a node in a sequence "
                    "(e.g. 'op A -> {op B; op C}'); bare children blocks are not allowed",
                    atom.span,
                )
                raise CompileError(ctx.bag)
            _reject_ignored_chain_tail(previous_lowered, atom.span, ctx)
            if previous_lowered.children is None:
                previous_lowered.children = []
            for item in atom.items:
                previous_lowered.children.append(_lower_atom(item, ctx))
            # Do not update previous_lowered. A children block constrains the
            # preceding matcher, not a chain link.
            continue

        lowered = _lower_atom(atom, ctx)
        if head_matcher is None:
            head_matcher = lowered
        else:
            if edge is ast_.Edge.PARENT:
                while previous_lowered.parent is not None:
                    previous_lowered = previous_lowered.parent
                _reject_ignored_chain_tail(previous_lowered, atom.span, ctx)
                previous_lowered.parent = lowered
            else:
                while previous_lowered.child is not None:
                    previous_lowered = previous_lowered.child
                _reject_ignored_chain_tail(previous_lowered, atom.span, ctx)
                previous_lowered.child = lowered
        previous_lowered = lowered

    return head_matcher


def _reject_ignored_chain_tail(host: LNode, span: Span, ctx: CompileCtx) -> None:
    if host.kind not in {"or", "recursive"}:
        return
    ctx.bag.error(
        "C009_MIXED_CHILD_PARENT_CHAIN_UNSUPPORTED",
        "a choice or recursive rule cannot have a child, children set, or parent chain tail; "
        "move the relation into each branch",
        span,
    )
    raise CompileError(ctx.bag)


def _check_redundant_exclusive_heads(p: ast_.Alternative, ctx: CompileCtx) -> None:
    """Reject explicit exclusive alternatives with duplicate concrete heads."""
    seen: dict[object, object] = {}
    for b in p.branches:
        if not isinstance(b, ast_.Node) or b.relations is not None:
            continue
        head = b.head
        if isinstance(head, ast_.OpHead):
            opcode_int = _resolve_opcode_ref(head.ref, ctx.resolved.opcodes, ctx.registries)
            key = ("op", opcode_int)
        elif isinstance(head, ast_.BTypeHead):
            key = ("btype", head.name)
        else:
            continue
        if key in seen:
            ctx.bag.error(
                "W202_EXCLUSIVE_OR_REDUNDANT_HEADS",
                f"two branches of this exclusive alternative share the same head "
                f"({'opcode' if key[0] == 'op' else 'btype'} {key[1]!r}); "
                f"use `or` for overlapping heads",
                b.span,
            )
        else:
            seen[key] = b.span


def _lnode_refs(n: LNode):
    for ref in (n.child, n.parent, n.step, n.exit_, n.target):
        if ref is not None:
            yield ref
    yield from n.children or ()
    yield from n.branches or ()


def _check_peek_subtree_closure(root: LNode, matcher_name: str, ctx: CompileCtx) -> None:
    all_nodes: list[LNode] = []
    seen_all: set[int] = set()
    stack = [root]
    while stack:
        node = stack.pop()
        if id(node) in seen_all:
            continue
        seen_all.add(id(node))
        all_nodes.append(node)
        stack.extend(_lnode_refs(node))

    for origin in all_nodes:
        if not origin.peek:
            continue
        seen = {id(origin)}
        descendants = list(_lnode_refs(origin))
        while descendants:
            node = descendants.pop()
            if id(node) in seen:
                continue
            seen.add(id(node))
            if not node.peek:
                if origin.peek_span is None:
                    raise AssertionError("peek node missing source span")
                ctx.bag.error(
                    "C014_PEEK_SUBTREE_CONSUMES",
                    f"matcher {matcher_name!r}: a `peek` subtree contains a consuming node; "
                    "every reachable node must also be `peek`",
                    origin.peek_span,
                )
                raise CompileError(ctx.bag)
            descendants.extend(_lnode_refs(node))


def _anchor_group_head_sets(
    group: ast_.Alternative,
    ctx: CompileCtx,
) -> tuple[frozenset[int], frozenset[str]]:
    """Collect the branch heads of an opcode-set anchor group.

    The resolver guarantees every branch is a bare Node with a concrete
    `op`/`btype` head and no `where` clause.
    """
    opcode_set: set[int] = set()
    btype_set: set[str] = set()
    for b in group.branches:
        head = b.head
        if isinstance(head, ast_.OpHead):
            opcode = _resolve_opcode_ref(head.ref, ctx.resolved.opcodes, ctx.registries)
            opcode_set.add(opcode)
        else:
            btype_set.add(head.name)
    return frozenset(opcode_set), frozenset(btype_set)


def _anchor_group_branch_descs(
    group: ast_.Alternative,
    ctx: CompileCtx,
) -> list[tuple[str, Any, str | None]]:
    """Per-branch descriptors of a set-membership anchor group, in source order.
    Each is `(kind, value, where_name)` where kind is `"op"`
    (value = resolved opcode int) or `"btype"` (value = btype name), and
    where_name is the branch's `where` predicate name or None. The resolver
    guarantees every branch is a bare concrete `op`/`btype` node."""
    descs: list[tuple[str, Any, str | None]] = []
    for b in group.branches:
        head = b.head
        if isinstance(head, ast_.OpHead):
            opcode = _resolve_opcode_ref(head.ref, ctx.resolved.opcodes, ctx.registries)
            descs.append(("op", opcode, b.where_predicate))
        else:
            descs.append(("btype", head.name, b.where_predicate))
    return descs


def _lower_atom(p: ast_.PatternExpr, ctx: CompileCtx) -> LNode:
    if isinstance(p, ast_.Many):
        inner = _lower_atom(p.inner, ctx)
        if inner.optional:
            ctx.bag.error("C017_MANY", "write `maybe many`, not `many maybe`", p.span)
            raise CompileError(ctx.bag)
        inner.many = True
        inner.max = p.max
        return inner
    if isinstance(p, ast_.Node):
        return _lower_node(p, ctx)
    if isinstance(p, ast_.Maybe):
        inner = _lower_atom(p.inner, ctx)
        inner.optional = True
        return inner
    if isinstance(p, ast_.Peek):
        inner = _lower_atom(p.inner, ctx)
        inner.peek = True
        inner.peek_span = p.span
        return inner
    if isinstance(p, ast_.Alternative):
        lowered_branches = [_lower_atom(b, ctx) for b in p.branches]
        if p.exclusive:
            _check_redundant_exclusive_heads(p, ctx)
            return LNode(kind="or", branches=lowered_branches, exclusive=True)
        if _alternative_is_exclusive(p, ctx.resolved.opcodes, ctx.registries):
            return LNode(kind="or", branches=lowered_branches, exclusive=True)
        return LNode(kind="or", branches=lowered_branches, exclusive=False)
    if isinstance(p, ast_.Sequence):
        pairs: list[tuple[ast_.Edge, ast_.PatternExpr]] = [(ast_.Edge.CHILD, p.head)]
        pairs.extend(p.tail)
        result = _lower_chain_with_children_attach(pairs, ctx)
        if result is None:
            raise AssertionError("Sequence lowered to None")
        return result
    if isinstance(p, ast_.RuleRef):
        target = next((r for r in ctx.resolved.raw.rules if r.name == p.name), None)
        if target is None:
            raise CompileError(ctx.bag)
        # _lower_cyclic_rule patches this back-edge to the shared body root.
        cyclic_pending = getattr(ctx, _CYCLIC_PENDING_KEY, None)
        if cyclic_pending is not None and p.name in cyclic_pending:
            ref = LNode(kind="cyclic_ref")
            cyclic_pending[p.name].append(ref)
            return ref
        # Self-reference inside the body of a recursive rule we're already lowering:
        # the frontier recursion node handles the repetition, so we represent the
        # self-ref as a permissive sentinel.
        if p.name in ctx.lowering_recursive:
            return LNode(kind="any")
        rec_class = ctx.resolved.rule_recursion.get(p.name)
        if rec_class is RecursionClass.DIRECT_RECURSIVE:
            if isinstance(target.pattern, ast_.Alternative) and target.pattern.exclusive:
                ctx.bag.error(
                    "C017_RECURSIVE_RULE_XOR",
                    f"rule {p.name!r}: a recursive rule body cannot be `xor`",
                    target.pattern.span,
                    suggestion="use `or`",
                )
                raise CompileError(ctx.bag)
            if target.strategy == "cyclic":
                return _lower_cyclic_rule(p.name, ctx)
            return _lower_recursive_rule(p.name, ctx)
        return _lower_atom(target.pattern, ctx)
    raise AssertionError(f"unhandled pattern node: {type(p).__name__}")


def _check_where_expr_sync(e: ast_.Expr, span: Span, ctx: CompileCtx) -> bool:
    """Compile-time mirror of the IR loader's _check_where_expr_sync.

    An inline `where (expr)` composes onto the engine's synchronous test_self,
    so async constructs (lookups, host fns) are compile errors here. The IR
    loader rejects the same artifacts, keeping both paths failing at the same
    compiler instead of silently never matching. Returns True
    when the expression is sync-evaluable."""
    if isinstance(e, ast_.NameRef):
        if e.name != ctx.where_anchor_capture:
            ctx.bag.error(
                "C015_WHERE_EXPR_CAPTURE_NOT_ENTRY",
                f"`where (expr)` may reference only anchor capture "
                f"@{ctx.where_anchor_capture}; name {e.name!r} is not allowed",
                e.span,
            )
            return False
        return True
    if isinstance(e, ast_.LookupExpr):
        ctx.bag.error(
            "C013_WHERE_EXPR_NOT_SYNC",
            "`where (expr)` cannot contain lookups (test_self is sync)",
            span,
        )
        return False
    if isinstance(e, ast_.Comprehension):
        ctx.bag.error(
            "C013_WHERE_EXPR_NOT_SYNC",
            "`where (expr)` cannot contain map/any/all/find/stable_sort_by comprehensions "
            "(build-expression only)",
            span,
        )
        return False
    if isinstance(e, ast_.ParseExpr):
        if not e.nullable:
            ctx.bag.error(
                "C013_WHERE_EXPR_NOT_SYNC",
                "`where (expr)` allows only `parse?`",
                span,
            )
            return False
        return _check_where_expr_sync(e.target, span, ctx)
    if isinstance(e, ast_.Call):
        callee = e.callee
        if callee.name not in BUILTIN_SIGNATURES:
            ctx.bag.error(
                "C013_WHERE_EXPR_NOT_SYNC",
                f"`where (expr)` can only call builtins, not host fn {callee.name!r}",
                span,
            )
            return False
        return all(_check_where_expr_sync(a, span, ctx) for a in e.args)
    if isinstance(e, ast_.FieldAccess):
        return _check_where_expr_sync(e.target, span, ctx)
    if isinstance(e, ast_.UnaryOp):
        return _check_where_expr_sync(e.operand, span, ctx)
    if isinstance(e, ast_.BinaryOp):
        return (_check_where_expr_sync(e.left, span, ctx)
                and _check_where_expr_sync(e.right, span, ctx))
    if isinstance(e, ast_.Ternary):
        return (_check_where_expr_sync(e.cond, span, ctx)
                and _check_where_expr_sync(e.then, span, ctx)
                and _check_where_expr_sync(e.orelse, span, ctx))
    if isinstance(e, ast_.ListLit):
        return all(_check_where_expr_sync(it, span, ctx) for it in e.elements)
    if isinstance(e, ast_.RecordLit):
        return all(_check_where_expr_sync(f.value, span, ctx) for f in e.fields)
    return True


def _set_where_expr(n: LNode, expr: ast_.Expr, span: Span, ctx: CompileCtx) -> None:
    """Store inline `where (expr)` on a node for IR emission.

    The C013 sync check still runs. An async construct
    (lookup/host fn) is a compile error, keeping the frontend and the IR loader
    failing at the same stage. The IR engine evaluates the expression at match
    time via the shared sync evaluator (expr_eval.py)."""
    if not _check_where_expr_sync(expr, span, ctx):
        return
    n.where_expr = expr


def _lower_node(n: ast_.Node, ctx: CompileCtx) -> LNode:
    head = n.head
    if isinstance(head, ast_.OpHead):
        opcode = _resolve_opcode_ref(head.ref, ctx.resolved.opcodes, ctx.registries)
        m = LNode(kind="contract", opcode=opcode)
    elif isinstance(head, ast_.BTypeHead):
        m = LNode(kind="block_type", btype=head.name)
    elif isinstance(head, ast_.PredHead):
        m = LNode(kind="pred", pred=head.name)
    elif isinstance(head, ast_.AnyHead):
        m = LNode(kind="any")
    else:
        raise AssertionError(f"unknown node head: {type(head).__name__}")
    if n.where_predicate is not None:
        m.where = n.where_predicate
    if n.where_expr is not None:
        _set_where_expr(m, n.where_expr, n.span, ctx)
    if n.capture is not None:
        m.capture = n.capture
    if n.relations is not None:
        _lower_relations(n.relations, m, ctx)
    return m


def _lower_relations(r: ast_.Relations, m: LNode, ctx: CompileCtx) -> None:
    if r.children_all is not None:
        m.child = _lower_atom(r.children_all, ctx)
    if r.children_first is not None:
        m.children = [_lower_atom(item, ctx) for item in r.children_first]
    if r.parent is not None:
        m.parent = _lower_atom(r.parent, ctx)


def _check_no_capture_after_recursion(
    pattern: ast_.PatternExpr,
    decl_span: Span,
    ctx: CompileCtx,
) -> None:
    """Reject captures after a direct-recursive reference in the same sequence.

    The walker cannot reach them after engine deduplication.
    """

    def is_direct_recursive_ref(p: ast_.PatternExpr) -> bool:
        if isinstance(p, ast_.RuleRef):
            return ctx.resolved.rule_recursion.get(p.name) is RecursionClass.DIRECT_RECURSIVE
        if isinstance(p, (ast_.Maybe, ast_.Peek, ast_.Many)):
            return is_direct_recursive_ref(p.inner)
        return False

    def contains_capture(p: ast_.PatternExpr) -> bool:
        if isinstance(p, ast_.RuleRef):
            target = next((r for r in ctx.resolved.raw.rules if r.name == p.name), None)
            return target is not None and contains_capture(target.pattern)
        return (
            isinstance(p, ast_.Node) and p.capture is not None
        ) or any(contains_capture(child) for child in ast_.children(p))

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
        else:
            for child in ast_.children(p):
                walk(child)

    walk(pattern)


def _check_children_block_placement(
    pattern: ast_.PatternExpr,
    ctx: CompileCtx,
) -> None:
    """Reject bare children blocks and edges after a children block."""

    def emit_c008(span: ast_.Span) -> None:
        ctx.bag.error(
            "C008_CHILDREN_BLOCK_BARE",
            "children block '{...}' must follow a node in a sequence "
            "(e.g. 'op A -> {op B; op C}'); bare children blocks are not allowed",
            span,
        )

    def emit_c009(span: ast_.Span) -> None:
        ctx.bag.error(
            "C009_MIXED_CHILD_PARENT_CHAIN_UNSUPPORTED",
            "an edge after a children block in the same chain is unsupported; "
            "put the relations on the preceding node in a relation block",
            span,
        )

    def walk(p: ast_.PatternExpr, in_chain_left: bool) -> None:
        """`in_chain_left` is True when this atom has a non-CB left sibling in its
        enclosing Sequence tail (i.e. it's a valid attach-to-left position).
        """
        if isinstance(p, ast_.ChildrenBlock) and not in_chain_left:
            emit_c008(p.span)
        if isinstance(p, ast_.Sequence):
            walk(p.head, in_chain_left=False)
            previous_was_atom = not _is_cb_atom(p.head)
            previous_was_children_block = _is_cb_atom(p.head)
            for _edge, atom in p.tail:
                if previous_was_children_block and previous_was_atom:
                    emit_c009(atom.span)
                walk(atom, in_chain_left=previous_was_atom)
                previous_was_children_block = _is_cb_atom(atom)
                if not previous_was_children_block:
                    previous_was_atom = True
        elif isinstance(p, ast_.RuleRef):
            # Check the rule body once at its definition site, with no chain
            # context. Its body must be self-contained or referenced from a
            # Sequence (in which case body's own bareness is independent).
            target = next((r for r in ctx.resolved.raw.rules if r.name == p.name), None)
            if target is not None and not _seen_rule(p.name, ctx):
                _mark_rule_seen(p.name, ctx)
                walk(target.pattern, in_chain_left=False)
        else:
            for child in ast_.children(p):
                walk(child, in_chain_left=False)

    walk(pattern, in_chain_left=False)


_RULE_VALIDATED_KEY = "__mch_rule_validated__"


def _seen_rule(name: str, ctx: CompileCtx) -> bool:
    seen: set[str] = getattr(ctx, _RULE_VALIDATED_KEY, set())
    return name in seen


def _mark_rule_seen(name: str, ctx: CompileCtx) -> None:
    seen: set[str] = getattr(ctx, _RULE_VALIDATED_KEY, set())
    seen.add(name)
    setattr(ctx, _RULE_VALIDATED_KEY, seen)


def _is_maybe_self_ref(p: ast_.PatternExpr, rule_name: str) -> bool:
    saw_maybe = False
    while isinstance(p, (ast_.Maybe, ast_.Peek, ast_.Many)):
        if isinstance(p, ast_.Maybe):
            saw_maybe = True
        p = p.inner
    return saw_maybe and isinstance(p, ast_.RuleRef) and p.name == rule_name


def _strip_trailing_self_ref(branch: ast_.PatternExpr, rule_name: str) -> ast_.PatternExpr | None:
    """Strip trailing `maybe $rule_name` from a step branch.

    Returns the stripped pattern, or None if the branch is just `maybe $rule_name`
    alone (the branch carries no actual content and should be discarded).
    """
    if _is_maybe_self_ref(branch, rule_name):
        return None

    def strip_relation_child(node: ast_.Node) -> ast_.Node:
        relations = node.relations
        if (
            relations is None
            or relations.children_all is None
            or not _is_maybe_self_ref(relations.children_all, rule_name)
        ):
            return node
        return replace(node, relations=replace(relations, children_all=None))

    if isinstance(branch, ast_.Node):
        return strip_relation_child(branch)
    if isinstance(branch, ast_.Sequence):
        if branch.tail:
            last_edge, last_atom = branch.tail[-1]
            if _is_maybe_self_ref(last_atom, rule_name):
                new_tail = branch.tail[:-1]
                if not new_tail:
                    return branch.head
                return replace(branch, tail=new_tail)
            if isinstance(last_atom, ast_.Node):
                stripped = strip_relation_child(last_atom)
                if stripped is not last_atom:
                    return replace(
                        branch,
                        tail=(*branch.tail[:-1], (last_edge, stripped)),
                    )
    return branch


# Dynamic CompileCtx attributes for cyclic rules avoid changing CompileCtx:
# pending back-references collected while a cyclic body is being lowered, and
# the per-compile cache of lowered body roots (shared across reference sites).
_CYCLIC_PENDING_KEY = "_mch_cyclic_pending"   # dict[str, list[LNode]]  (cyclic_ref back-edges)
_CYCLIC_ROOTS_KEY = "_mch_cyclic_roots"       # dict[str, LNode]  (shared body roots)


def _check_cyclic_rule_body(target: ast_.RuleDecl, ctx: CompileCtx) -> None:
    """Enforce C012 shape rules for cyclic rule bodies.
    """
    name = target.name

    def is_self_ref(a: ast_.PatternExpr) -> bool:
        if isinstance(a, (ast_.Maybe, ast_.Peek, ast_.Many)):
            return is_self_ref(a.inner)
        return isinstance(a, ast_.RuleRef) and a.name == name

    def err(msg: str, span: ast_.Span) -> None:
        ctx.bag.error("C012_CYCLIC_RULE_SHAPE", f"rule {name!r}: {msg}", span)
        raise CompileError(ctx.bag)

    def walk(p: ast_.PatternExpr) -> None:
        if isinstance(p, ast_.Node):
            if p.capture is not None:
                err(
                    f"capture @{p.capture} inside a cyclic rule body never binds; "
                    f"use `consumed` in output expressions",
                    p.span,
                )
            for child in ast_.children(p):
                walk(child)
        elif isinstance(p, ast_.Sequence):
            atoms = [p.head] + [a for _e, a in p.tail]
            for i, atom in enumerate(atoms):
                if is_self_ref(atom) and i != len(atoms) - 1:
                    err(
                        "a self-reference must be the last atom of its chain "
                        "(atoms after it would be structurally ignored)",
                        atom.span,
                    )
                walk(atom)
        elif isinstance(p, (ast_.Alternative, ast_.ChildrenBlock)):
            for child in ast_.children(p):
                walk(child)
        elif isinstance(p, ast_.Maybe):
            if is_self_ref(p.inner):
                err(
                    "`maybe` on a self-reference inside a cyclic body is not "
                    "supported (an IR back-edge carries no per-site optionality); "
                    "add a non-recursive alternative branch instead",
                    p.span,
                )
            walk(p.inner)
        elif isinstance(p, (ast_.Peek, ast_.Many)):
            if is_self_ref(p.inner):
                err(
                    "`peek` on a self-reference inside a cyclic body is not "
                    "supported (an IR back-edge carries no per-site modifiers); "
                    "put `peek` on concrete nodes instead",
                    p.span,
                )
            walk(p.inner)
        elif isinstance(p, ast_.RuleRef):
            if p.name == name:
                return
            rc = ctx.resolved.rule_recursion.get(p.name)
            if rc in (RecursionClass.DIRECT_RECURSIVE, RecursionClass.INDIRECT_RECURSIVE):
                err(
                    f"reference to recursive rule {p.name!r} inside a cyclic "
                    f"rule body is not supported (nested recursion)",
                    p.span,
                )
            other = next((r for r in ctx.resolved.raw.rules if r.name == p.name), None)
            if other is not None:
                walk(other.pattern)  # non-recursive rules inline; hold them to the same rules

    walk(target.pattern)


def _lower_cyclic_rule(name: str, ctx: CompileCtx) -> LNode:
    """Lower a cyclic rule reference.

    The body lowers once per compile into a shared node graph; in-body
    self-references become back-edges to the body root (patched here after the
    body finishes lowering). Each reference site emits its own cyclic `recursive`
    node whose `step` is the shared body root; several sites may therefore share
    one body subgraph.
    """
    target = next(r for r in ctx.resolved.raw.rules if r.name == name)
    roots: dict[str, LNode] = getattr(ctx, _CYCLIC_ROOTS_KEY, None) or {}
    if not hasattr(ctx, _CYCLIC_ROOTS_KEY):
        setattr(ctx, _CYCLIC_ROOTS_KEY, roots)
    root = roots.get(name)
    if root is None:
        _check_cyclic_rule_body(target, ctx)
        pending: dict = getattr(ctx, _CYCLIC_PENDING_KEY, None)
        if pending is None:
            pending = {}
            setattr(ctx, _CYCLIC_PENDING_KEY, pending)
        pending[name] = []
        try:
            root = _lower_atom(target.pattern, ctx)
        finally:
            back_refs = pending.pop(name)
        if root.kind == "cyclic_ref":
            ctx.bag.error(
                "C012_CYCLIC_RULE_SHAPE",
                f"rule {name!r}: body reduces to a bare self-reference",
                target.span,
            )
            raise CompileError(ctx.bag)
        for ref in back_refs:
            ref.target = root
        roots[name] = root
    return LNode(kind="recursive", strategy="cyclic", step=root)


def _lower_recursive_rule(name: str, ctx: CompileCtx) -> LNode:
    """Lower a recursive rule to a frontier `recursive` node.

    Strip trailing `maybe $self` from step branches; leave `exit_` None when
    there are no pure-exit branches (avoids the v0 'always-one-iteration' bug
    where an always-true exit short-circuits the loop). Frontier recursion nodes
    always carry `optional=True`.
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
            step = LNode(
                kind="or",
                branches=[_lower_atom(b, ctx) for b in stripped_step_branches],
                exclusive=False,
            )

        if has_pure_exit:
            if len(exit_branches) == 1:
                exit_m: LNode | None = _lower_atom(exit_branches[0], ctx)
            else:
                exit_m = LNode(
                    kind="or",
                    branches=[_lower_atom(b, ctx) for b in exit_branches],
                    exclusive=False,
                )
        else:
            exit_m = None
    finally:
        ctx.lowering_recursive.discard(name)

    return LNode(kind="recursive", step=step, exit_=exit_m, optional=True)


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
    # Splitting a tail Alternative would discard the sequence head and other atoms.
    if isinstance(pattern, ast_.Sequence):
        for _e, atom in pattern.tail:
            if isinstance(atom, ast_.Alternative):
                return [], []
    return [pattern], []


def _contains_rule_ref(p: ast_.PatternExpr, name: str) -> bool:
    if isinstance(p, ast_.RuleRef):
        return p.name == name
    return any(_contains_rule_ref(child, name) for child in ast_.children(p))
