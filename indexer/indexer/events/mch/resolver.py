from __future__ import annotations

from dataclasses import dataclass, fields, is_dataclass, replace
from enum import Enum
from typing import Iterable

from indexer.events.mch import ast_, diagnostics
from indexer.events.mch.btypes import CLASS_TO_BTYPE, is_class_name
from indexer.events.mch.builtin_signatures import BUILTIN_SIGNATURES
from indexer.events.mch.diagnostics import CompileError, DiagnosticBag, Span
from indexer.events.mch.registry import Registries


class RecursionClass(Enum):
    NONE = "NONE"
    DIRECT_RECURSIVE = "DIRECT_RECURSIVE"
    INDIRECT_RECURSIVE = "INDIRECT_RECURSIVE"


@dataclass
class CaptureInfo:
    name: str
    head: ast_.NodeHead
    optional: bool
    peek: bool
    list_binding: bool
    source: ast_.Node


@dataclass
class _CaptureOccurrence:
    """One @name position in a matcher pattern, with enough context to decide
    whether several positions sharing a name are mutually exclusive."""
    node: ast_.Node
    under_maybe: bool
    under_peek: bool
    under_recursive: bool
    branch_path: tuple[tuple[int, int, int], ...]
    alt_path: tuple[tuple[int, int, int], ...]
    # True when any enclosing alternative is NOT exclusive (plain `|` failing the
    # ExclusiveOr-upgrade heuristic). Such positions can never anchor.
    under_nonexclusive_alt: bool = False


def _resolve_opcode_ref(ref: str | int, opcodes: dict[str, int], registries: Registries) -> int | None:
    if isinstance(ref, int):
        return ref
    return opcodes.get(ref) if ref in opcodes else registries.opcodes.get(ref)


def _alternative_is_exclusive(
    alt: ast_.Alternative,
    opcodes: dict[str, int],
    registries: Registries,
) -> bool:
    """True when at most one branch can match a given block.

    Explicit `^` alternatives are exclusive by declaration. `|` alternatives are
    exclusive when they meet the compiler's ExclusiveOrMatcher upgrade heuristic:
    every branch is a bare concrete node (`op`/`btype`), no `where` clause, and
    all resolved heads are pairwise distinct.
    """
    if alt.exclusive:
        return True
    seen_ops: set[int] = set()
    seen_btypes: set[str] = set()
    for b in alt.branches:
        if not isinstance(b, ast_.Node) or b.where_predicate is not None \
                or b.where_expr is not None or b.relations is not None:
            return False
        head = b.head
        if isinstance(head, ast_.OpHead):
            op = _resolve_opcode_ref(head.ref, opcodes, registries)
            if op is None or op in seen_ops:
                return False
            seen_ops.add(op)
        elif isinstance(head, ast_.BTypeHead):
            if head.name in seen_btypes:
                return False
            seen_btypes.add(head.name)
        else:
            return False
    return True


def _occurrences_mutually_exclusive(a: _CaptureOccurrence, b: _CaptureOccurrence) -> bool:
    """True if a and b sit in different branches of some common alternative."""
    b_by_alt = {alt_id: idx for alt_id, idx, _n in b.alt_path}
    for alt_id, idx, _n in a.alt_path:
        if alt_id in b_by_alt and b_by_alt[alt_id] != idx:
            return True
    return False


def _merged_optional(occs: list[_CaptureOccurrence]) -> bool:
    """Optionality of a capture defined in several mutually exclusive positions.

    The capture always binds (optional=False) only when no occurrence is under
    `maybe` and some exclusive alternative common to ALL occurrences is fully
    covered, with one occurrence in each branch. Otherwise a branch without
    the capture can win and the field stays at its default.
    """
    if any(o.under_maybe for o in occs):
        return True
    common_alts = set.intersection(
        *[{alt_id for alt_id, _i, _n in o.branch_path} for o in occs]
    )
    for alt_id in common_alts:
        idxs: list[int] = []
        branch_count = 0
        for o in occs:
            for a_id, idx, n in o.branch_path:
                if a_id == alt_id:
                    idxs.append(idx)
                    branch_count = n
        if len(idxs) == len(set(idxs)) == branch_count:
            return False
    return True


def _validate_group_anchor(
    occs: list[_CaptureOccurrence],
    alt_nodes: dict[int, ast_.Alternative],
) -> tuple[ast_.Alternative | None, str]:
    """Validate an `entry` capture that touches alternative branches.

    Legal as a set-membership anchor: every occurrence is itself a branch of one
    exclusive alternative, one per branch, all branches covered, each branch a
    bare concrete `op`/`btype` node. Heads may mix `op` and `btype` (union
    membership) and each branch may carry a named `where <pred>` clause.
    Membership is tested first, then the matching branch's predicate. Returns
    (group, "") on success or (None, reason) for the R007 message.
    """
    if any(o.under_recursive for o in occs):
        return None, "is inside a recursive rule"
    if any(o.under_maybe for o in occs):
        return None, "is inside a `maybe`-wrapped subtree"
    if any(o.under_peek for o in occs):
        return None, "is inside a `peek`-wrapped subtree"
    if any(o.under_nonexclusive_alt for o in occs):
        return None, (
            "sits in a non-exclusive alternative branch; an opcode-set anchor "
            "requires an exclusive alternative (`^`, or `|` of distinct bare "
            "concrete heads)"
        )
    if any(len(o.branch_path) != 1 for o in occs):
        return None, (
            "must sit directly on the branches of a single exclusive "
            "alternative, not nested in or outside one"
        )
    alt_id = occs[0].branch_path[0][0]
    if any(o.branch_path[0][0] != alt_id for o in occs):
        return None, (
            "occurrences span different alternatives; an opcode-set anchor "
            "requires a single exclusive alternative"
        )
    alt = alt_nodes[alt_id]
    idxs = {o.branch_path[0][1] for o in occs}
    if len(occs) != len(alt.branches) or idxs != set(range(len(alt.branches))):
        return None, (
            "must be bound on every branch of the exclusive alternative"
        )
    by_idx = {o.branch_path[0][1]: o for o in occs}
    for idx, branch in enumerate(alt.branches):
        occ = by_idx[idx]
        if branch is not occ.node or (occ.node.relations is not None and not alt.opcode_set):
            return None, (
                "requires every branch of the exclusive alternative to be a "
                "bare node carrying the capture (no chains or nesting)"
            )
        if not isinstance(occ.node.head, (ast_.OpHead, ast_.BTypeHead)):
            return None, (
                "requires concrete `op`/`btype` heads on every branch of the "
                "exclusive alternative"
            )
    return alt, ""


@dataclass
class ResolvedMatcher:
    decl: ast_.MatcherDecl
    captures: dict[str, CaptureInfo]
    anchor_capture: str
    anchor_node: ast_.Node
    # When `entry` names a capture bound on
    # every branch of one exclusive alternative of bare concrete heads, this is
    # that Alternative; `anchor_node` then holds a representative branch node.
    anchor_group: ast_.Alternative | None = None
    build_stmts: tuple[ast_.BuildStmt, ...] = ()


@dataclass
class ResolvedFile:
    raw: ast_.File
    opcodes: dict[str, int]
    rule_recursion: dict[str, RecursionClass]
    matchers: tuple[ResolvedMatcher, ...]
    diagnostics: DiagnosticBag


def resolve(file: ast_.File, registries: Registries) -> ResolvedFile:
    bag = DiagnosticBag()
    file = _inline_fns(file, registries, bag)

    opcodes: dict[str, int] = _resolve_opcodes(file, registries, bag)
    _check_predicates(file, registries, bag)
    _check_produces_union(file, registries, bag)
    _check_shapers(file, registries, bag)
    _check_rule_patterns(file, opcodes, registries, bag)
    rule_recursion: dict[str, RecursionClass] = _classify_rules(file, bag)
    resolved_matchers: list[ResolvedMatcher] = []
    for m in file.matchers:
        resolved_matchers.append(_resolve_matcher(m, opcodes, file, registries, bag, rule_recursion))

    _check_priorities(file, bag)

    if bag.has_errors:
        raise CompileError(bag)

    return ResolvedFile(
        raw=file,
        opcodes=opcodes,
        rule_recursion=rule_recursion,
        matchers=tuple(resolved_matchers),
        diagnostics=bag,
    )


def _map_fn_children(node, visit):
    if isinstance(node, tuple):
        return tuple(visit(item) for item in node)
    if is_dataclass(node) and not isinstance(node, Span):
        return replace(node, **{
            f.name: visit(getattr(node, f.name)) for f in fields(node) if f.name != "span"
        })
    return node


def _inline_fns(file: ast_.File, registries: Registries, bag: DiagnosticBag) -> ast_.File:
    if not file.fns:
        return file
    fns: dict[str, ast_.FnDecl] = {}

    def check_body(node, params):
        if isinstance(node, ast_.NameRef) and node.name not in params:
            bag.error(diagnostics.R031_FN_BODY_FREE_NAME,
                      f"fn body reference {node.name!r} is not a parameter", node.span)
        elif isinstance(node, ast_.LookupExpr):
            bag.error(diagnostics.R032_FN_BODY_LOOKUP,
                      "fn bodies cannot contain lookup; bind it with `let`", node.span)
            return node
        elif isinstance(node, ast_.Comprehension):
            bag.error(diagnostics.R033_FN_BODY_COMPREHENSION,
                      "fn bodies cannot contain comprehensions", node.span)
            return node
        elif isinstance(node, ast_.Call) and isinstance(node.callee, ast_.NameRef):
            check_body(node.args, params)
            return node
        return _map_fn_children(node, lambda child: check_body(child, params))

    for fn in file.fns:
        if fn.name in fns:
            bag.error(diagnostics.R034_DUP_FN,
                      f"fn {fn.name!r} declared more than once in this file", fn.span)
            continue
        fns[fn.name] = fn
        if fn.name in BUILTIN_SIGNATURES or fn.name in registries.fns:
            bag.error(diagnostics.R035_FN_NAME_COLLISION,
                      f"fn {fn.name!r} collides with a builtin or registered host function", fn.span)
        check_body(fn.body, fn.params)
    if bag.has_errors:
        raise CompileError(bag)

    bodies: dict[str, ast_.Expr] = {}
    active: set[str] = set()

    def expand_body(name):
        if name in active:
            bag.error(diagnostics.R037_FN_CYCLE,
                      f"fn {name!r} participates in a call cycle", fns[name].span)
            raise CompileError(bag)
        if name not in bodies:
            active.add(name)
            bodies[name] = inline(fns[name].body, {})
            active.remove(name)
        return bodies[name]

    def inline(node, bindings):
        if isinstance(node, ast_.NameRef):
            return bindings.get(node.name, node)
        if isinstance(node, ast_.Call) and isinstance(node.callee, ast_.NameRef):
            name = node.callee.name
            if name in fns and len(node.args) != len(fns[name].params):
                bag.error(diagnostics.R036_FN_ARITY,
                          f"fn {name!r} expects {len(fns[name].params)} arguments, got {len(node.args)}",
                          node.span)
                raise CompileError(bag)
            args = tuple(inline(arg, bindings) for arg in node.args)
            if name in fns:
                return inline(expand_body(name), dict(zip(fns[name].params, args)))
            return replace(node, args=args)
        return _map_fn_children(node, lambda child: inline(child, bindings))

    for name in fns:
        expand_body(name)
    return replace(file, rules=inline(file.rules, {}), matchers=inline(file.matchers, {}))


def _resolve_opcodes(file: ast_.File, registries: Registries, bag: DiagnosticBag) -> dict[str, int]:
    opcodes = {"Excess": 0xd53276db, "Bounce": 0xffffffff}
    seen: set[str] = set()
    for o in file.opcodes:
        if o.name in seen:
            bag.error(
                "R005_DUP_CAPTURE",  # reuse code; or define R009
                f"opcode {o.name!r} declared more than once in this file",
                o.span,
            )
            continue
        seen.add(o.name)
        if o.name in opcodes:
            if o.value == opcodes[o.name]:
                report = bag.error if o.exported else bag.warning
                report(
                    "R025_BUILTIN_OPCODE_REDECLARED",
                    f"opcode {o.name!r} is built in; drop the declaration",
                    o.span,
                )
            else:
                bag.error(
                    "R026_BUILTIN_OPCODE_MISMATCH",
                    f"opcode {o.name!r} is built in as {hex(opcodes[o.name])}, "
                    f"not {hex(o.value)}",
                    o.span,
                )
        if o.name in registries.opcodes and registries.opcodes[o.name] != o.value:
            bag.error(
                "R002_OPCODE_VALUE_MISMATCH",
                f"opcode {o.name!r} declared as {hex(o.value)} but host registry has {hex(registries.opcodes[o.name])}",
                o.span,
            )
        opcodes[o.name] = o.value

    reported: set[Span] = set()

    def visit(p: ast_.PatternExpr) -> None:
        if isinstance(p, ast_.Alternative):
            if p.opcode_set:
                members: dict[int, Span] = {}
                for branch in p.branches:
                    node = branch.head if isinstance(branch, ast_.Sequence) else branch
                    head = node.head
                    value = _resolve_opcode_ref(head.ref, opcodes, registries)
                    if value is None:
                        continue
                    value &= 0xffffffff
                    if value in members:
                        for span in (members[value], head.span):
                            if span not in reported:
                                bag.error("R027_DUPLICATE_OPCODE_SET_MEMBER",
                                          f"opcode set repeats unsigned opcode {hex(value)}", span)
                                reported.add(span)
                    else:
                        members[value] = head.span
            for branch in p.branches:
                visit(branch)
        elif isinstance(p, (ast_.Maybe, ast_.Peek)):
            visit(p.inner)
        elif isinstance(p, ast_.Sequence):
            visit(p.head)
            for _edge, atom in p.tail:
                visit(atom)
        elif isinstance(p, ast_.ChildrenBlock):
            for item in p.items:
                visit(item)
        elif isinstance(p, ast_.Node) and p.relations is not None:
            r = p.relations
            for target in (r.children_all, *(r.children_first or ()), r.parent):
                if target is not None:
                    visit(target)

    for decl in (*file.rules, *file.matchers):
        visit(decl.pattern)
    return opcodes


def _check_predicates(file: ast_.File, registries: Registries, bag: DiagnosticBag) -> None:
    for p in file.predicates:
        if p.name not in registries.predicates:
            bag.error(
                "R003_UNKNOWN_PREDICATE",
                f"predicate {p.name!r} is not registered in the host",
                p.span,
            )


def _is_real_block_class(cls: object) -> bool:
    """True for a concrete typed block class (a `Block` subclass other than the
    base). Stubs (`object`, used by the emit stub registries and the synthetic
    test matchers) and the generic `Block` itself carry their btype as the
    produced name, so they keep the identity fallback and are not R022 cases."""
    from indexer.events.blocks.core import Block  # lazy: keep resolver import-light
    return isinstance(cls, type) and issubclass(cls, Block) and cls is not Block


def _check_produces_union(file: ast_.File, registries: Registries, bag: DiagnosticBag) -> None:
    """Validate every name in a matcher's `produces` union against
    the host's block-type registry. A single-name `produces` is a 1-element union.

    Also validates that a PascalCase `produces` name normalizes to a real btype
    via `btypes.CLASS_TO_BTYPE`. Without an entry the name would fall through to
    the class-name string as the produced btype, causing a
    silent downstream mismatch (the serializer and legacy blocks key on the real
    btype). Snake_case names are bare btypes and need no entry."""
    for m in file.matchers:
        for name in m.produces:
            if name not in registries.block_types:
                bag.error(
                    "C003_UNKNOWN_BLOCK_TYPE",
                    f"matcher {m.name!r}: produces type {name!r} is not registered",
                    m.span,
                )
            elif (
                is_class_name(name)
                and name not in CLASS_TO_BTYPE
                and _is_real_block_class(registries.block_types[name])
            ):
                # Typed block classes must normalize to the engine's btype.
                bag.error(
                    diagnostics.R022_UNNORMALIZED_PRODUCES_CLASS,
                    f"matcher {m.name!r}: declarative `produces` class {name!r} "
                    f"has no btype mapping in mch/btypes.py CLASS_TO_BTYPE — it "
                    f"would silently emit {name!r} as the produced btype instead "
                    f"of the block's real btype. Add the class->btype entry, or "
                    f"use the bare btype string in `produces`.",
                    m.span,
                )


def _check_shapers(file: ast_.File, registries: Registries, bag: DiagnosticBag) -> None:
    """Validate every `shape` directive against the host's shaper registry
    (same early-check pattern as `_check_produces_union`; the compiler re-checks)."""
    for m in file.matchers:
        if m.shape is not None and m.shape not in registries.shapers:
            bag.error(
                "C011_UNKNOWN_SHAPER",
                f"matcher {m.name!r}: shaper {m.shape!r} is not registered in the host",
                m.span,
            )


def _classify_rules(file: ast_.File, bag: DiagnosticBag) -> dict[str, RecursionClass]:
    name_to_rule = {r.name: r for r in file.rules}
    out: dict[str, RecursionClass] = {}

    def collect_refs(p: ast_.PatternExpr, refs: list, under_maybe: bool) -> None:
        """Append (ref_name, inside_maybe) for each $X reference within pattern p."""
        if isinstance(p, ast_.RuleRef):
            refs.append((p.name, under_maybe))
        elif isinstance(p, ast_.Maybe):
            collect_refs(p.inner, refs, True)
        elif isinstance(p, ast_.Peek):
            collect_refs(p.inner, refs, under_maybe)
        elif isinstance(p, ast_.Alternative):
            for b in p.branches:
                collect_refs(b, refs, under_maybe)
        elif isinstance(p, ast_.Sequence):
            collect_refs(p.head, refs, under_maybe)
            for _e, atom in p.tail:
                collect_refs(atom, refs, under_maybe)
        elif isinstance(p, ast_.ChildrenBlock):
            for it in p.items:
                collect_refs(it, refs, under_maybe)
        elif isinstance(p, ast_.Node) and p.relations is not None:
            r = p.relations
            for target in (r.children_all, *(r.children_first or ()), r.parent):
                if target is not None:
                    collect_refs(target, refs, under_maybe)

    for r in file.rules:
        refs = []
        collect_refs(r.pattern, refs, under_maybe=False)
        for ref_name, _ in refs:
            if ref_name not in name_to_rule:
                bag.error(
                    "R004_UNDEFINED_RULE",
                    f"rule {ref_name!r} referenced but not defined",
                    r.span,
                )

    for r in file.rules:
        refs = []
        collect_refs(r.pattern, refs, under_maybe=False)
        direct_self = any(name == r.name for name, _ in refs)
        if direct_self:
            # Frontier recursion needs at least one maybe-guarded $self to
            # terminate. Cyclic recursion terminates through a non-recursive
            # alternative. `maybe $self` is forbidden there by C012; descent is
            # bounded by trace depth, so R006 does not apply.
            terminating = any(name == r.name and inside_maybe for name, inside_maybe in refs)
            if not terminating and r.strategy != "cyclic":
                bag.error(
                    "R006_UNBOUNDED_RECURSION",
                    f"rule {r.name!r} is directly self-recursive without a `maybe $rule` termination",
                    r.span,
                )
            out[r.name] = RecursionClass.DIRECT_RECURSIVE
        else:
            out[r.name] = RecursionClass.NONE

    # Indirect recursion (mutual). Walk the graph; mark any rule that's part of a cycle and
    # not already DIRECT_RECURSIVE.
    adj: dict[str, set[str]] = {r.name: set() for r in file.rules}
    for r in file.rules:
        refs = []
        collect_refs(r.pattern, refs, under_maybe=False)
        for n, _ in refs:
            if n in adj:
                adj[r.name].add(n)

    def in_cycle(start: str) -> bool:
        stack = [(start, iter(adj[start]))]
        seen = {start}
        while stack:
            node, it = stack[-1]
            try:
                nxt = next(it)
            except StopIteration:
                stack.pop()
                continue
            if nxt == start and len(stack) >= 1:
                return True
            if nxt in seen:
                continue
            seen.add(nxt)
            stack.append((nxt, iter(adj[nxt])))
        return False

    for r in file.rules:
        if out.get(r.name) is RecursionClass.DIRECT_RECURSIVE:
            continue
        if in_cycle(r.name):
            out[r.name] = RecursionClass.INDIRECT_RECURSIVE

    return out


def _check_rule_patterns(
    file: ast_.File,
    opcodes: dict[str, int],
    registries: Registries,
    bag: DiagnosticBag,
) -> None:
    """Validate opcode references and inline where-predicates in all rule patterns."""

    def visit(p: ast_.PatternExpr) -> None:
        if isinstance(p, ast_.Node):
            head = p.head
            if isinstance(head, ast_.OpHead) and isinstance(head.ref, str):
                if head.ref not in opcodes and head.ref not in registries.opcodes:
                    bag.error(
                        "R001_UNKNOWN_OPCODE",
                        f"opcode {head.ref!r} is not declared in this file or in the host registry",
                        head.span,
                    )
            if p.where_predicate and p.where_predicate not in registries.predicates:
                bag.error(
                    "R003_UNKNOWN_PREDICATE",
                    f"predicate {p.where_predicate!r} is not registered in the host",
                    p.span,
                )
            if p.relations is not None:
                r = p.relations
                for target in (r.children_all, *(r.children_first or ()), r.parent):
                    if target is not None:
                        visit(target)
        elif isinstance(p, (ast_.Maybe, ast_.Peek)):
            visit(p.inner)
        elif isinstance(p, ast_.Sequence):
            visit(p.head)
            for _e, atom in p.tail:
                visit(atom)
        elif isinstance(p, ast_.Alternative):
            for b in p.branches:
                visit(b)
        elif isinstance(p, ast_.ChildrenBlock):
            for it in p.items:
                visit(it)

    for r in file.rules:
        visit(r.pattern)


def _resolve_matcher(
    m: ast_.MatcherDecl,
    opcodes: dict[str, int],
    file: ast_.File,
    registries: Registries,
    bag: DiagnosticBag,
    rule_recursion: dict[str, RecursionClass],
) -> ResolvedMatcher:
    occurrences: dict[str, list[_CaptureOccurrence]] = {}
    alt_ids: dict[int, int] = {}  # id(Alternative) -> stable sequential id
    alt_nodes: dict[int, ast_.Alternative] = {}  # stable id -> Alternative node
    relation_depth = 0

    def visit(
        p: ast_.PatternExpr,
        under_maybe: bool,
        under_peek: bool,
        under_recursive: bool,
        branch_path: tuple[tuple[int, int, int], ...],
        alt_path: tuple[tuple[int, int, int], ...],
        under_nonexclusive_alt: bool = False,
    ) -> None:
        nonlocal relation_depth
        if isinstance(p, ast_.Node):
            head = p.head
            if isinstance(head, ast_.OpHead) and isinstance(head.ref, str):
                if head.ref not in opcodes and head.ref not in registries.opcodes:
                    bag.error(
                        "R001_UNKNOWN_OPCODE",
                        f"opcode {head.ref!r} is not declared in this file or in the host registry",
                        head.span,
                    )
            if p.where_predicate and p.where_predicate not in registries.predicates:
                bag.error(
                    "R003_UNKNOWN_PREDICATE",
                    f"predicate {p.where_predicate!r} is not registered in the host",
                    p.span,
                )
            if p.capture is not None:
                if p.capture == m.entry and relation_depth:
                    bag.error(
                        "R007_ENTRY_VIOLATES_8_3",
                        f"matcher {m.name!r}: entry @{m.entry} sits inside a relation block; make that node the entry",
                        p.span,
                    )
                occurrences.setdefault(p.capture, []).append(_CaptureOccurrence(
                    node=p,
                    under_maybe=under_maybe,
                    under_peek=under_peek,
                    under_recursive=under_recursive,
                    branch_path=branch_path,
                    alt_path=alt_path,
                    under_nonexclusive_alt=under_nonexclusive_alt,
                ))
            if p.relations is not None:
                r = p.relations
                relation_depth += 1
                for target in (r.children_all, *(r.children_first or ()), r.parent):
                    if target is not None:
                        visit(target, under_maybe, under_peek, under_recursive,
                              branch_path, alt_path, under_nonexclusive_alt)
                relation_depth -= 1
        elif isinstance(p, ast_.Maybe):
            visit(
                p.inner, True, under_peek, under_recursive, branch_path, alt_path,
                under_nonexclusive_alt,
            )
        elif isinstance(p, ast_.Peek):
            visit(
                p.inner, under_maybe, True, under_recursive, branch_path, alt_path,
                under_nonexclusive_alt,
            )
        elif isinstance(p, ast_.Sequence):
            visit(
                p.head, under_maybe, under_peek, under_recursive, branch_path, alt_path,
                under_nonexclusive_alt,
            )
            for _e, atom in p.tail:
                visit(
                    atom, under_maybe, under_peek, under_recursive, branch_path, alt_path,
                    under_nonexclusive_alt,
                )
        elif isinstance(p, ast_.Alternative):
            exclusive = _alternative_is_exclusive(p, opcodes, registries)
            alt_id = alt_ids.setdefault(id(p), len(alt_ids))
            if exclusive:
                alt_nodes[alt_id] = p
            for idx, b in enumerate(p.branches):
                branch = ((alt_id, idx, len(p.branches)),)
                visit(b, under_maybe, under_peek, under_recursive,
                      branch_path + branch if exclusive else branch_path,
                      alt_path + branch,
                      under_nonexclusive_alt or not exclusive)
        elif isinstance(p, ast_.ChildrenBlock):
            for it in p.items:
                visit(
                    it, under_maybe, under_peek, under_recursive, branch_path, alt_path,
                    under_nonexclusive_alt,
                )
        elif isinstance(p, ast_.RuleRef):
            # Recurse into the rule body. If we're entering a recursive rule, set
            # under_recursive=True for the duration of the walk. Stop following
            # references into recursive rules (direct or indirect) once we've
            # crossed the recursive boundary, to avoid infinite descent on mutual
            # cycles.
            target = next((r for r in file.rules if r.name == p.name), None)
            if target is not None and not under_recursive:
                target_class = rule_recursion.get(target.name, RecursionClass.NONE)
                if target_class is RecursionClass.NONE:
                    visit(target.pattern, under_maybe, under_peek, under_recursive=False,
                          branch_path=branch_path, alt_path=alt_path,
                          under_nonexclusive_alt=under_nonexclusive_alt)
                else:
                    visit(target.pattern, under_maybe, under_peek, under_recursive=True,
                          branch_path=branch_path, alt_path=alt_path,
                          under_nonexclusive_alt=under_nonexclusive_alt)

    visit(
        m.pattern,
        under_maybe=False,
        under_peek=False,
        under_recursive=False,
        branch_path=(),
        alt_path=(),
    )

    prelude: dict[str, ast_.ParseStmt] = {}
    captures: dict[str, CaptureInfo] = {}
    for cap_name, occs in occurrences.items():
        for o in occs:
            node = o.node
            if node.parse_types is None:
                continue
            if not isinstance(node.head, ast_.OpHead):
                bag.error(
                    diagnostics.R030_PARSE_ANNOTATION_ON_COMPOSED,
                    "parse annotations require an `op` node; read composed fields via `.data`",
                    node.span,
                )
            stmt = ast_.ParseStmt(cap_name, node.parse_types, node.span)
            if prelude.setdefault(cap_name, stmt).msg_types != node.parse_types:
                bag.error(
                    diagnostics.R028_PARSE_ANNOTATION_CONFLICT,
                    f"capture {cap_name!r} has conflicting parse annotations",
                    node.span,
                )
        if len(occs) > 1:
            # Every pair must occupy different branches of a common alternative
            # and agree on recursion context.
            same_recursion = all(o.under_recursive == occs[0].under_recursive for o in occs)
            offending: ast_.Node | None = None if same_recursion else occs[1].node
            if offending is None:
                for i in range(len(occs)):
                    for j in range(i + 1, len(occs)):
                        if not _occurrences_mutually_exclusive(occs[i], occs[j]):
                            offending = occs[j].node
                            break
                    if offending is not None:
                        break
            if offending is not None:
                bag.error(
                    "R005_DUP_CAPTURE",
                    f"capture {cap_name!r} defined more than once in matcher {m.name!r} "
                    f"(occurrences are not in different branches of a common alternative)",
                    offending.span,
                )
                first = occs[0]
                captures[cap_name] = CaptureInfo(
                    name=cap_name,
                    head=first.node.head,
                    optional=first.under_maybe,
                    peek=first.under_peek,
                    list_binding=first.under_recursive,
                    source=first.node,
                )
                continue
            captures[cap_name] = CaptureInfo(
                name=cap_name,
                head=occs[0].node.head,
                optional=_merged_optional(occs),
                peek=any(o.under_peek for o in occs),
                list_binding=occs[0].under_recursive,
                source=occs[0].node,
            )
        else:
            o = occs[0]
            captures[cap_name] = CaptureInfo(
                name=cap_name,
                head=o.node.head,
                optional=o.under_maybe or bool(o.branch_path),
                peek=o.under_peek,
                list_binding=o.under_recursive,
                source=o.node,
            )

    anchor_group: ast_.Alternative | None = None
    if m.entry is None:
        bag.error(
            "R008_ENTRY_REQUIRED",
            f"matcher {m.name!r} must declare `entry @<capture>` in v0",
            m.span,
        )
        anchor_capture = ""
        anchor_node: ast_.Node = m.pattern  # type: ignore[assignment]
    else:
        anchor_capture = m.entry
        if m.entry not in captures:
            bag.error(
                "R007_ENTRY_VIOLATES_8_3",
                f"matcher {m.name!r}: entry @{m.entry} references an undefined capture",
                m.span,
            )
            anchor_node = m.pattern  # type: ignore[assignment]
        else:
            ci = captures[m.entry]
            occs = occurrences.get(m.entry, [])
            if len(occs) > 1 or any(o.branch_path or o.under_nonexclusive_alt for o in occs):
                # Multi-position or alternative-resident entry: legal only as an
                # Opcode-set anchor over one exclusive alternative.
                anchor_group, reason = _validate_group_anchor(occs, alt_nodes)
                if anchor_group is None:
                    bag.error(
                        "R007_ENTRY_VIOLATES_8_3",
                        f"matcher {m.name!r}: entry @{m.entry} {reason}",
                        m.span,
                    )
            else:
                if ci.optional:
                    bag.error(
                        "R007_ENTRY_VIOLATES_8_3",
                        f"matcher {m.name!r}: entry @{m.entry} is inside a `maybe`-wrapped subtree",
                        m.span,
                    )
                if ci.peek:
                    bag.error(
                        "R007_ENTRY_VIOLATES_8_3",
                        f"matcher {m.name!r}: entry @{m.entry} is inside a `peek`-wrapped subtree",
                        m.span,
                    )
                if ci.list_binding:
                    bag.error(
                        "R007_ENTRY_VIOLATES_8_3",
                        f"matcher {m.name!r}: entry @{m.entry} is inside a recursive rule",
                        m.span,
                    )
                if not isinstance(ci.head, (ast_.OpHead, ast_.BTypeHead, ast_.PredHead)):
                    bag.error(
                        "R007_ENTRY_VIOLATES_8_3",
                        f"matcher {m.name!r}: entry @{m.entry} must reference an "
                        f"`op`, `btype`, or `pred` node",
                        m.span,
                    )
                # A `pred`-headed anchor has no opcode/btype
                # prefilter: the engine full-scans `test_self` (the predicate) on
                # every candidate block.
            anchor_node = ci.source

    for stmt in m.build_stmts:
        if isinstance(stmt, ast_.ParseStmt) and stmt.capture in prelude:
            bag.error(
                diagnostics.R029_PARSE_ANNOTATION_AND_STATEMENT,
                f"capture {stmt.capture!r} has both a parse annotation and a parse statement",
                stmt.span,
            )
    if prelude:
        m = replace(m, build_stmts=tuple(prelude.values()) + m.build_stmts)
    _check_build_stmts(m, captures, registries, bag)

    return ResolvedMatcher(
        decl=m,
        captures=captures,
        anchor_capture=anchor_capture,
        anchor_node=anchor_node,
        anchor_group=anchor_group,
        build_stmts=m.build_stmts,
    )


# Env names the synthesized declarative build path always binds (compiler
# `build_block`: `consumed` = deduped consumed-block list, `anchor` = anchor
# block). They resolve like captures in build expressions, and `let` may not
# shadow them.
WELL_KNOWN_ENV: frozenset[str] = frozenset({"consumed", "anchor"})


def _check_build_stmts(
    m: ast_.MatcherDecl,
    captures: dict[str, CaptureInfo],
    registries: Registries,
    bag: DiagnosticBag,
) -> None:
    """Build-statement validation.

    - Every bare value reference must resolve to a pattern capture or a `let`
      declared earlier (R010). Builtin callees are intentionally unvalidated.
    - `parse` targets must be captures (R011) and name registered message
      types (R012).
    - `let` names must not collide with captures or earlier lets (R013).
    - `out` fields must be unique (R014); at most one `out` block (R017).
      it may sit anywhere among the statements, evaluation stays source-order.
    - Matchers must have an `out` or `produces switch` (R016).
    """
    has_out = any(isinstance(s, ast_.OutStmt) for s in m.build_stmts)
    has_switch = any(isinstance(s, ast_.SwitchStmt) for s in m.build_stmts)
    if not (has_out or has_switch):
        bag.error(
            "R016_BUILD_NO_OUT",
            f"matcher {m.name!r} has no `out` block or "
            f"`produces switch`; nothing to produce",
            m.span,
        )
    if has_out and has_switch:
        bag.error(
            "R017_MULTIPLE_OUT",
            f"matcher {m.name!r} mixes an `out` block and a `produces switch`; "
            f"use exactly one output mechanism",
            m.span,
        )

    # Well-known env names (`consumed`, `anchor`) are always bound by the
    # synthesized build path. They are legal references, and `let` may not shadow them.
    available: set[str] = set(captures.keys()) | set(WELL_KNOWN_ENV)
    out_seen = False
    for stmt in m.build_stmts:
        if isinstance(stmt, ast_.ParseStmt):
            if stmt.capture not in captures:
                bag.error(
                    "R011_PARSE_TARGET_NOT_CAPTURE",
                    f"parse target {stmt.capture!r} is not a pattern capture",
                    stmt.span,
                )
            elif (isinstance(captures[stmt.capture].head, ast_.BTypeHead)
                  and captures[stmt.capture].head.name not in
                  ("ton_transfer", "call_contract", "tick_tock")):
                bag.error(
                    diagnostics.R023_PARSE_TARGET_COMPOSED_BLOCK,
                    f"parse target {stmt.capture!r} is a produced block "
                    f"(captured by `btype {captures[stmt.capture].head.name}`); "
                    f"only base blocks (ton_transfer, call_contract, tick_tock) can be parsed. "
                    f"Read its fields via `{stmt.capture}.data.<field>` instead.",
                    stmt.span,
                )
            for t in stmt.msg_types:
                if t not in registries.message_types:
                    bag.error(
                        "R012_UNKNOWN_MESSAGE_TYPE",
                        f"message type {t!r} is not registered in the host",
                        stmt.span,
                    )
        elif isinstance(stmt, ast_.LetStmt):
            _check_expr_refs(stmt.value, available, bag, registries)
            if stmt.name in available:
                bag.error(
                    "R013_DUP_LET",
                    f"`let {stmt.name}` collides with a capture or an earlier `let`",
                    stmt.span,
                )
            available.add(stmt.name)
        elif isinstance(stmt, (ast_.RejectStmt, ast_.FailedStmt, ast_.BrokenStmt)):
            _check_expr_refs(stmt.condition, available, bag, registries)
        elif isinstance(stmt, ast_.OutStmt):
            if out_seen:
                bag.error(
                    "R017_MULTIPLE_OUT",
                    f"matcher {m.name!r} has more than one `out` block",
                    stmt.span,
                )
            out_seen = True
            _check_out_fields(stmt.fields, available, bag, registries)
        elif isinstance(stmt, ast_.SwitchStmt):
            else_seen = False
            for i, branch in enumerate(stmt.branches):
                if branch.condition is not None:
                    if else_seen:
                        bag.error(
                            "R019_SWITCH_BRANCH_AFTER_ELSE",
                            f"produces switch: `when` branch after the `else` arm is unreachable",
                            branch.span,
                        )
                    _check_expr_refs(branch.condition, available, bag, registries)
                else:
                    if else_seen:
                        bag.error(
                            "R019_SWITCH_BRANCH_AFTER_ELSE",
                            f"produces switch: more than one `else` arm",
                            branch.span,
                        )
                    else_seen = True
                if branch.btype not in m.produces:
                    bag.error(
                        "R020_SWITCH_BTYPE_NOT_DECLARED",
                        f"produces switch branch btype {branch.btype!r} is not in the "
                        f"matcher's `produces` union {list(m.produces)}",
                        branch.span,
                    )
                _check_out_fields(branch.out.fields, available, bag, registries)
            if not else_seen:
                bag.error(
                    "R021_SWITCH_NO_ELSE",
                    f"produces switch has no `else` arm (exhaustiveness cannot be "
                    f"proven; add an `else`)",
                    stmt.span,
                )


def _check_priorities(file: ast_.File, bag: DiagnosticBag) -> None:
    """Warn when two matchers share a NON-default `priority`: their
    relative order then falls back to source position, usually not the author's
    intent. Default-100 matchers are the norm and never warned."""
    seen: dict[int, str] = {}
    for m in file.matchers:
        if m.priority == 100:
            continue
        if m.priority in seen:
            bag.warning(
                "W010_DUPLICATE_PRIORITY",
                f"matchers {seen[m.priority]!r} and {m.name!r} both declare "
                f"priority {m.priority}; their order falls back to source position",
                m.span,
            )
        else:
            seen[m.priority] = m.name


def _check_out_fields(fields: tuple[ast_.OutField, ...], available: set[str],
                      bag: DiagnosticBag, registries: Registries) -> None:
    """Unique field names (R014) and resolvable value expressions, for an `out`
    block or a `produces switch` branch's out."""
    seen_fields: set[str] = set()
    for f in fields:
        if f.name in seen_fields:
            bag.error(
                "R014_DUP_OUT_FIELD",
                f"out field {f.name!r} appears more than once",
                f.span,
            )
        seen_fields.add(f.name)
        _check_expr_refs(f.value, available, bag, registries)


def _check_expr_refs(expr: ast_.Expr, available: set[str], bag: DiagnosticBag,
                     registries: Registries) -> None:
    """Report each value-position NameRef not in `available`. A bare NameRef used
    as a call callee is a function/builtin name and is skipped.

    A `map|any|all(xs as e => body)` comprehension binds `e` in `body` only (a
    new scope frame); `body` may shadow an outer capture named `e`. A
    comprehension nested in another comprehension's `xs` or `body` is R024."""
    def value(e: ast_.Expr, avail: set[str], in_comp: bool) -> None:
        if isinstance(e, ast_.NameRef):
            if e.name not in avail:
                bag.error(
                    "R010_UNRESOLVED_REFERENCE",
                    f"reference {e.name!r} does not resolve to a capture or a "
                    f"previously declared `let`",
                    e.span,
                )
        elif isinstance(e, ast_.FieldAccess):
            value(e.target, avail, in_comp)
        elif isinstance(e, ast_.Call):
            if not isinstance(e.callee, ast_.NameRef):
                value(e.callee, avail, in_comp)
            for a in e.args:
                value(a, avail, in_comp)
        elif isinstance(e, ast_.LookupExpr):
            for a in e.args:
                value(a, avail, in_comp)
        elif isinstance(e, ast_.UnaryOp):
            value(e.operand, avail, in_comp)
        elif isinstance(e, ast_.BinaryOp):
            value(e.left, avail, in_comp)
            value(e.right, avail, in_comp)
        elif isinstance(e, ast_.Ternary):
            value(e.cond, avail, in_comp)
            value(e.then, avail, in_comp)
            value(e.orelse, avail, in_comp)
        elif isinstance(e, ast_.ListLit):
            for x in e.elements:
                value(x, avail, in_comp)
        elif isinstance(e, ast_.RecordLit):
            for f in e.fields:
                value(f.value, avail, in_comp)
        elif isinstance(e, ast_.ParseExpr):
            # The `parse <target> as T` target is a value expression; the
            # message types must name registered host message parsers (R012, same
            # as the `parse` build statement).
            value(e.target, avail, in_comp)
            for t in e.msg_types:
                if t not in registries.message_types:
                    bag.error(
                        "R012_UNKNOWN_MESSAGE_TYPE",
                        f"message type {t!r} is not registered in the host",
                        e.span,
                    )
        elif isinstance(e, ast_.Comprehension):
            if in_comp:
                bag.error(
                    diagnostics.R024_NESTED_COMPREHENSION,
                    "nested comprehensions are not allowed; a `map|any|all` "
                    "comprehension may not appear inside another comprehension",
                    e.span,
                )
            # `xs` evaluates in the enclosing scope (no binder); `body` sees the
            # element var, which may shadow an outer capture.
            value(e.xs, avail, True)
            value(e.body, avail | {e.var}, True)

    value(expr, available, False)


def _is_recursive_set(file: ast_.File) -> set[str]:
    """Helper: names of rules that self-reference (direct or via cycle)."""
    out: set[str] = set()
    name_to_rule = {r.name: r for r in file.rules}

    def collect_refs(p: ast_.PatternExpr, refs: set[str]) -> None:
        if isinstance(p, ast_.RuleRef):
            refs.add(p.name)
        elif isinstance(p, (ast_.Maybe, ast_.Peek)):
            collect_refs(p.inner, refs)
        elif isinstance(p, ast_.Alternative):
            for b in p.branches:
                collect_refs(b, refs)
        elif isinstance(p, ast_.Sequence):
            collect_refs(p.head, refs)
            for _e, atom in p.tail:
                collect_refs(atom, refs)
        elif isinstance(p, ast_.ChildrenBlock):
            for it in p.items:
                collect_refs(it, refs)
        elif isinstance(p, ast_.Node) and p.relations is not None:
            r = p.relations
            for target in (r.children_all, *(r.children_first or ()), r.parent):
                if target is not None:
                    collect_refs(target, refs)

    for r in file.rules:
        refs: set[str] = set()
        collect_refs(r.pattern, refs)
        if r.name in refs:
            out.add(r.name)

    return out
