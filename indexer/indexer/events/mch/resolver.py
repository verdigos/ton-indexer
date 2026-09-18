from __future__ import annotations

from dataclasses import dataclass, fields, is_dataclass, replace
from enum import Enum
from typing import Iterable

from indexer.events.mch import ast_, diagnostics
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
    under_many: bool
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
    group: ast_.Alternative,
    opcodes: dict[str, int],
    registries: Registries,
) -> tuple[ast_.Node | None, str, str]:
    """Validate an alternative used as a matcher's head."""
    if not _alternative_is_exclusive(group, opcodes, registries):
        return None, "", (
            "is a non-exclusive alternative; a group head requires an "
            "exclusive alternative (`xor`, or `or` of distinct bare concrete heads)"
        )
    first = group.branches[0]
    capture = first.capture if isinstance(first, ast_.Node) else None
    if capture is None:
        return None, "", "requires one shared capture on every branch"
    for branch in group.branches:
        if not isinstance(branch, ast_.Node) or (branch.relations is not None and not group.opcode_set):
            return None, "", (
                "requires every branch to be a bare node carrying the shared "
                "capture (no chains or nesting)"
            )
        if not isinstance(branch.head, (ast_.OpHead, ast_.BTypeHead)):
            return None, "", "requires concrete `op`/`btype` heads on every branch"
        if branch.capture != capture:
            return None, "", "requires the same capture on every branch"
    return first, capture, ""


@dataclass
class ResolvedMatcher:
    decl: ast_.MatcherDecl
    captures: dict[str, CaptureInfo]
    anchor_capture: str
    anchor_node: ast_.Node
    # For an exclusive alternative of bare concrete heads, this is that
    # Alternative; `anchor_node` holds a representative branch node.
    anchor_group: ast_.Alternative | None = None


@dataclass
class ResolvedFile:
    raw: ast_.File
    opcodes: dict[str, int]
    rule_recursion: dict[str, RecursionClass]
    matchers: tuple[ResolvedMatcher, ...]


def resolve(file: ast_.File, registries: Registries) -> ResolvedFile:
    bag = DiagnosticBag()
    file = _inline_fns(file, registries, bag)

    opcodes: dict[str, int] = _resolve_opcodes(file, registries, bag)
    for kind, declarations in (("rule", file.rules), ("matcher", file.matchers)):
        seen: set[str] = set()
        for decl in declarations:
            if decl.name in seen:
                bag.error("R005_DUP_CAPTURE", f"duplicate {kind} {decl.name!r}", decl.span)
            seen.add(decl.name)
    _check_named_where_predicates(file, bag)
    _check_rule_patterns(file, opcodes, registries, bag)
    rule_recursion: dict[str, RecursionClass] = _classify_rules(file, bag)
    resolved_matchers: list[ResolvedMatcher] = []
    for m in file.matchers:
        resolved_matchers.append(_resolve_matcher(m, opcodes, file, registries, bag, rule_recursion))

    if bag.items:
        raise CompileError(bag)

    return ResolvedFile(
        raw=file,
        opcodes=opcodes,
        rule_recursion=rule_recursion,
        matchers=tuple(resolved_matchers),
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
        elif isinstance(node, ast_.Comprehension):
            bag.error(diagnostics.R033_FN_BODY_COMPREHENSION,
                      "fn bodies cannot contain comprehensions", node.span)
            return node
        elif isinstance(node, ast_.Call) and isinstance(node.callee, ast_.NameRef):
            check_body(node.args, params)
            return node
        return _map_fn_children(node, lambda child: check_body(child, params))

    for fn in file.fns:
        fns[fn.name] = fn
        if fn.name in BUILTIN_SIGNATURES or fn.name in registries.fns:
            bag.error(diagnostics.R035_FN_NAME_COLLISION,
                      f"fn {fn.name!r} collides with a builtin or registered host function", fn.span)
        check_body(fn.body, fn.params)
    if bag.items:
        raise CompileError(bag)

    bodies: dict[str, ast_.Expr] = {}

    def expand_body(name):
        if name not in bodies:
            bodies[name] = inline(fns[name].body, {})
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
    return replace(
        file,
        fns=tuple(replace(fn, body=expand_body(fn.name)) for fn in file.fns),
        rules=inline(file.rules, {}),
        matchers=inline(file.matchers, {}),
    )


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
        if o.name in opcodes and o.value != opcodes[o.name]:
            bag.error(
                "R026_BUILTIN_OPCODE_MISMATCH",
                f"opcode {o.name!r} is built in as {hex(opcodes[o.name])}, "
                f"not {hex(o.value)}",
                o.span,
            )
        opcodes[o.name] = o.value

    return opcodes


def _check_named_where_predicates(file: ast_.File, bag: DiagnosticBag) -> None:
    declared = {predicate.name for predicate in file.predicates}

    def visit(p: ast_.PatternExpr) -> None:
        if (isinstance(p, ast_.Node) and p.where_predicate is not None
                and p.where_predicate not in declared):
            bag.error(
                diagnostics.R044_UNDECLARED_PREDICATE,
                f"predicate {p.where_predicate!r} used by `where` is not declared",
                p.span,
            )
        for child in ast_.children(p):
            visit(child)

    for declaration in (*file.rules, *file.matchers):
        visit(declaration.pattern)


def _classify_rules(file: ast_.File, bag: DiagnosticBag) -> dict[str, RecursionClass]:
    name_to_rule = {r.name: r for r in file.rules}
    out: dict[str, RecursionClass] = {}

    def collect_refs(p: ast_.PatternExpr, refs: list, under_maybe: bool) -> None:
        """Append (ref_name, inside_maybe) for each $X reference within pattern p."""
        if isinstance(p, ast_.RuleRef):
            refs.append((p.name, under_maybe))
        elif isinstance(p, ast_.Maybe):
            collect_refs(ast_.children(p)[0], refs, True)
        else:
            for child in ast_.children(p):
                collect_refs(child, refs, under_maybe)

    for r in (*file.rules, *file.matchers):
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
        for child in ast_.children(p):
            visit(child)

    for r in file.rules:
        visit(r.pattern)


def _check_many(pattern, file, recursion, bag):
    rules = {r.name: r.pattern for r in file.rules}

    def walk(p, item=False, repeated=False, head=False, peek=False, recursive=False, path=()):
        def error(message):
            bag.error("C017_MANY", message, p.span)
            raise CompileError(bag)

        if isinstance(p, ast_.Many):
            if recursive:
                error("`many` cannot be under or contain a recursive rule")
            inner = p.inner.head if isinstance(p.inner, ast_.Sequence) else p.inner
            if isinstance(inner, ast_.Maybe):
                error("write `maybe many`, not `many maybe`")
            if not item:
                error("`many` requires a children-set item; `->` already takes every match; "
                      "write `-> { many X @xs }` for a list")
            walk(p.inner, repeated=True, head=not repeated, path=path)
        elif isinstance(p, (ast_.Maybe, ast_.Peek)):
            if repeated and isinstance(p, ast_.Maybe):
                head = False
            walk(p.inner, item, repeated, head, peek or isinstance(p, ast_.Peek), recursive, path)
        elif isinstance(p, ast_.Node):
            if repeated and p.capture and not head:
                error("a capture inside `many` must sit on the repeated block")
            if p.relations:
                r = p.relations
                for target in (r.children_all, r.parent):
                    if target is not None:
                        walk(target, repeated=repeated, peek=peek, recursive=recursive, path=path)
                for target in r.children_first or ():
                    walk(target, True, repeated, peek=peek, recursive=recursive, path=path)
        elif isinstance(p, ast_.Sequence):
            walk(p.head, item, repeated, head, peek, recursive, path)
            previous = p.head
            for _, target in p.tail:
                while isinstance(previous, (ast_.Maybe, ast_.Peek, ast_.Many)):
                    previous = previous.inner
                recursive |= (isinstance(previous, ast_.RuleRef) and
                              recursion.get(previous.name, RecursionClass.NONE) is not RecursionClass.NONE)
                walk(target, repeated=repeated, peek=peek, recursive=recursive, path=path)
                previous = target
        elif isinstance(p, ast_.Alternative):
            for branch in ast_.children(p):
                walk(branch, repeated=repeated, head=head, peek=peek, recursive=recursive, path=path)
        elif isinstance(p, ast_.ChildrenBlock):
            for target in ast_.children(p):
                walk(target, True, repeated, peek=peek, recursive=recursive, path=path)
        elif isinstance(p, ast_.RuleRef) and p.name in rules:
            rec = recursion.get(p.name, RecursionClass.NONE) is not RecursionClass.NONE
            if repeated and rec:
                error("`many` cannot be under or contain a recursive rule")
            if p.name not in path:
                walk(rules[p.name], item, repeated, head, peek, recursive or rec, path + (p.name,))

    walk(pattern)


def _resolve_matcher(
    m: ast_.MatcherDecl,
    opcodes: dict[str, int],
    file: ast_.File,
    registries: Registries,
    bag: DiagnosticBag,
    rule_recursion: dict[str, RecursionClass],
) -> ResolvedMatcher:
    _check_many(m.pattern, file, rule_recursion, bag)
    occurrences: dict[str, list[_CaptureOccurrence]] = {}
    alt_ids: dict[int, int] = {}  # id(Alternative) -> stable sequential id
    under_many = False

    def visit(
        p: ast_.PatternExpr,
        under_maybe: bool,
        under_peek: bool,
        under_recursive: bool,
        branch_path: tuple[tuple[int, int, int], ...],
        alt_path: tuple[tuple[int, int, int], ...],
        under_nonexclusive_alt: bool = False,
    ) -> None:
        nonlocal under_many
        if isinstance(p, ast_.Node):
            head = p.head
            if isinstance(head, ast_.OpHead) and isinstance(head.ref, str):
                if head.ref not in opcodes and head.ref not in registries.opcodes:
                    bag.error(
                        "R001_UNKNOWN_OPCODE",
                        f"opcode {head.ref!r} is not declared in this file or in the host registry",
                        head.span,
                    )
            if p.capture is not None:
                occurrences.setdefault(p.capture, []).append(_CaptureOccurrence(
                    node=p,
                    under_maybe=under_maybe,
                    under_peek=under_peek,
                    under_recursive=under_recursive,
                    under_many=under_many,
                    branch_path=branch_path,
                    alt_path=alt_path,
                    under_nonexclusive_alt=under_nonexclusive_alt,
                ))
            for child in ast_.children(p):
                visit(child, under_maybe, under_peek, under_recursive,
                      branch_path, alt_path, under_nonexclusive_alt)
        elif isinstance(p, ast_.Many):
            saved = under_many
            under_many = True
            visit(p.inner, under_maybe, under_peek, under_recursive,
                  branch_path, alt_path, under_nonexclusive_alt)
            under_many = saved
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
            for child in ast_.children(p):
                visit(
                    child, under_maybe, under_peek, under_recursive, branch_path, alt_path,
                    under_nonexclusive_alt,
                )
        elif isinstance(p, ast_.Alternative):
            exclusive = _alternative_is_exclusive(p, opcodes, registries)
            alt_id = alt_ids.setdefault(id(p), len(alt_ids))
            for idx, b in enumerate(p.branches):
                branch = ((alt_id, idx, len(p.branches)),)
                visit(b, under_maybe, under_peek, under_recursive,
                      branch_path + branch if exclusive else branch_path,
                      alt_path + branch,
                      under_nonexclusive_alt or not exclusive)
        elif isinstance(p, ast_.ChildrenBlock):
            for child in ast_.children(p):
                visit(
                    child, under_maybe, under_peek, under_recursive, branch_path, alt_path,
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
            same_recursion = all((o.under_recursive, o.under_many) ==
                                 (occs[0].under_recursive, occs[0].under_many) for o in occs)
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
                    list_binding=first.under_recursive or first.under_many,
                    source=first.node,
                )
                continue
            captures[cap_name] = CaptureInfo(
                name=cap_name,
                head=occs[0].node.head,
                optional=_merged_optional(occs),
                peek=any(o.under_peek for o in occs),
                list_binding=occs[0].under_recursive or occs[0].under_many,
                source=occs[0].node,
            )
        else:
            o = occs[0]
            captures[cap_name] = CaptureInfo(
                name=cap_name,
                head=o.node.head,
                optional=o.under_maybe or bool(o.branch_path),
                peek=o.under_peek,
                list_binding=o.under_recursive or o.under_many,
                source=o.node,
            )

    head = m.pattern.head if isinstance(m.pattern, ast_.Sequence) else m.pattern
    anchor_group: ast_.Alternative | None = None
    if isinstance(head, ast_.Sequence):
        bag.error(
            "C009_MIXED_CHILD_PARENT_CHAIN_UNSUPPORTED",
            "a grouped sequence cannot be the head of another chain; write "
            "`op A @a -> op B -> op C` without grouping",
            head.span,
        )
        raise CompileError(bag)
    if isinstance(head, ast_.Alternative):
        anchor_node, anchor_capture, reason = _validate_group_anchor(head, opcodes, registries)
        if anchor_node is None:
            bag.error(
                "R007_ENTRY_VIOLATES_8_3",
                f"matcher {m.name!r}: head group {reason}",
                head.span,
            )
            raise CompileError(bag)
        anchor_group = head
    elif isinstance(head, ast_.Node) and isinstance(
        head.head, (ast_.OpHead, ast_.BTypeHead, ast_.PredHead)
    ):
        anchor_node = head
        anchor_capture = head.capture or ""
    elif isinstance(head, ast_.ChildrenBlock):
        bag.error(
            "C008_CHILDREN_BLOCK_BARE",
            "children block '{...}' must follow a node in a sequence "
            "(e.g. 'op A -> {op B; op C}'); bare children blocks are not allowed",
            head.span,
        )
        raise CompileError(bag)
    else:
        reason = {
            ast_.Maybe: "cannot be `maybe`-wrapped",
            ast_.Peek: "cannot be `peek`-wrapped",
            ast_.Many: "cannot be `many`-wrapped",
            ast_.RuleRef: "cannot be a rule reference",
        }.get(type(head), "must be an `op`, `btype`, `pred`, or eligible group")
        bag.error(
            "R007_ENTRY_VIOLATES_8_3",
            f"matcher {m.name!r}: head {reason}",
            head.span,
        )
        raise CompileError(bag)

    if prelude:
        m = replace(m, build_stmts=tuple(prelude.values()) + m.build_stmts)
    _check_build_stmts(m, captures, registries, bag)

    return ResolvedMatcher(
        decl=m,
        captures=captures,
        anchor_capture=anchor_capture,
        anchor_node=anchor_node,
        anchor_group=anchor_group,
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

    # Well-known env names (`consumed`, `anchor`) are always bound by the
    # synthesized build path. They are legal references, and `let` may not shadow them.
    available: set[str] = set(captures.keys()) | set(WELL_KNOWN_ENV)
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
                    diagnostics.R023_PARSE_TARGET_NOT_BASE_BLOCK,
                    f"parse target {stmt.capture!r} is a produced block "
                    f"(captured by `btype {captures[stmt.capture].head.name}`); "
                    f"only base blocks (ton_transfer, call_contract, tick_tock) can be parsed. "
                    f"Read its fields via `{stmt.capture}.data.<field>` instead.",
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
        elif isinstance(stmt, (ast_.RejectStmt, ast_.FailedStmt)):
            _check_expr_refs(stmt.condition, available, bag, registries)
        elif isinstance(stmt, ast_.OutStmt):
            _check_out_fields(stmt.fields, available, bag, registries)
        elif isinstance(stmt, ast_.SwitchStmt):
            else_seen = False
            for i, branch in enumerate(stmt.branches):
                if branch.condition is not None:
                    _check_expr_refs(branch.condition, available, bag, registries)
                else:
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
            value(e.target, avail, in_comp)
        elif isinstance(e, ast_.Comprehension):
            if in_comp:
                bag.error(
                    diagnostics.R024_NESTED_COMPREHENSION,
                    "nested comprehensions are not allowed; a `map|any|all|find|stable_sort_by` "
                    "comprehension may not appear inside another comprehension",
                    e.span,
                )
            # `xs` evaluates in the enclosing scope (no binder); `body` sees the
            # element var, which may shadow an outer capture.
            value(e.xs, avail, True)
            value(e.body, avail | {e.var}, True)

    value(expr, available, False)
