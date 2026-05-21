from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Iterable

from indexer.events.mch import ast_
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
    list_binding: bool
    source: ast_.Node


@dataclass
class ResolvedMatcher:
    decl: ast_.MatcherDecl
    captures: dict[str, CaptureInfo]
    anchor_capture: str
    anchor_node: ast_.Node


@dataclass
class ResolvedFile:
    raw: ast_.File
    opcodes: dict[str, int]
    rule_recursion: dict[str, RecursionClass]
    matchers: tuple[ResolvedMatcher, ...]
    stitches: tuple[ast_.StitchDecl, ...]
    diagnostics: DiagnosticBag


def resolve(file: ast_.File, registries: Registries) -> ResolvedFile:
    bag = DiagnosticBag()

    opcodes: dict[str, int] = _resolve_opcodes(file, registries, bag)
    _check_predicates(file, registries, bag)
    _check_rule_patterns(file, opcodes, registries, bag)
    rule_recursion: dict[str, RecursionClass] = _classify_rules(file, bag)
    resolved_matchers: list[ResolvedMatcher] = []
    for m in file.matchers:
        resolved_matchers.append(_resolve_matcher(m, opcodes, file, registries, bag, rule_recursion))

    if file.stitches:
        for s in file.stitches:
            bag.warning(
                "W900_STITCH_DEFERRED",
                f"stitch declarations are accepted but not lowered in v0",
                s.span,
            )

    if bag.has_errors:
        raise CompileError(bag)

    return ResolvedFile(
        raw=file,
        opcodes=opcodes,
        rule_recursion=rule_recursion,
        matchers=tuple(resolved_matchers),
        stitches=file.stitches,
        diagnostics=bag,
    )


def _resolve_opcodes(file: ast_.File, registries: Registries, bag: DiagnosticBag) -> dict[str, int]:
    seen: dict[str, int] = {}
    for o in file.opcodes:
        if o.name in seen:
            bag.error(
                "R005_DUP_CAPTURE",  # reuse code; or define R009
                f"opcode {o.name!r} declared more than once in this file",
                o.span,
            )
            continue
        if o.name in registries.opcodes and registries.opcodes[o.name] != o.value:
            bag.error(
                "R002_OPCODE_VALUE_MISMATCH",
                f"opcode {o.name!r} declared as {hex(o.value)} but host registry has {hex(registries.opcodes[o.name])}",
                o.span,
            )
        seen[o.name] = o.value
    return seen


def _check_predicates(file: ast_.File, registries: Registries, bag: DiagnosticBag) -> None:
    for p in file.predicates:
        if p.name not in registries.predicates:
            bag.error(
                "R003_UNKNOWN_PREDICATE",
                f"predicate {p.name!r} is not registered in the host",
                p.span,
            )


def _classify_rules(file: ast_.File, bag: DiagnosticBag) -> dict[str, RecursionClass]:
    # Build the rule reference graph; classify cycles.
    name_to_rule = {r.name: r for r in file.rules}
    out: dict[str, RecursionClass] = {}

    def collect_refs(p: ast_.PatternExpr, refs: list, under_maybe: bool) -> None:
        """Append (ref_name, inside_maybe) for each $X reference within pattern p."""
        if isinstance(p, ast_.RuleRef):
            refs.append((p.name, under_maybe))
        elif isinstance(p, ast_.Maybe):
            collect_refs(p.inner, refs, True)
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
        # Node: no refs.

    # First validate all $X targets exist.
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

    # Classify each rule.
    for r in file.rules:
        refs = []
        collect_refs(r.pattern, refs, under_maybe=False)
        direct_self = any(name == r.name for name, _ in refs)
        if direct_self:
            # Must have at least one maybe-guarded $self for the recursion to terminate.
            terminating = any(name == r.name and inside_maybe for name, inside_maybe in refs)
            if not terminating:
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
        elif isinstance(p, ast_.Maybe):
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
        # RuleRef has no inline head to validate here.

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
    # Walk the pattern; bind captures with their flags; validate op/predicate refs.
    captures: dict[str, CaptureInfo] = {}

    def visit(p: ast_.PatternExpr, under_maybe: bool, under_recursive: bool) -> None:
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
                if p.capture in captures:
                    bag.error(
                        "R005_DUP_CAPTURE",
                        f"capture {p.capture!r} defined more than once in matcher {m.name!r}",
                        p.span,
                    )
                else:
                    captures[p.capture] = CaptureInfo(
                        name=p.capture,
                        head=head,
                        optional=under_maybe,
                        list_binding=under_recursive,
                        source=p,
                    )
        elif isinstance(p, ast_.Maybe):
            visit(p.inner, True, under_recursive)
        elif isinstance(p, ast_.Sequence):
            visit(p.head, under_maybe, under_recursive)
            for _e, atom in p.tail:
                visit(atom, under_maybe, under_recursive)
        elif isinstance(p, ast_.Alternative):
            for b in p.branches:
                visit(b, under_maybe, under_recursive)
        elif isinstance(p, ast_.ChildrenBlock):
            for it in p.items:
                visit(it, under_maybe, under_recursive)
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
                    visit(target.pattern, under_maybe, under_recursive=False)
                else:
                    visit(target.pattern, under_maybe, under_recursive=True)

    visit(m.pattern, under_maybe=False, under_recursive=False)

    # Anchor.
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
            if ci.optional:
                bag.error(
                    "R007_ENTRY_VIOLATES_8_3",
                    f"matcher {m.name!r}: entry @{m.entry} is inside a `maybe`-wrapped subtree",
                    m.span,
                )
            if ci.list_binding:
                bag.error(
                    "R007_ENTRY_VIOLATES_8_3",
                    f"matcher {m.name!r}: entry @{m.entry} is inside a recursive rule",
                    m.span,
                )
            if not isinstance(ci.head, (ast_.OpHead, ast_.BTypeHead)):
                bag.error(
                    "R007_ENTRY_VIOLATES_8_3",
                    f"matcher {m.name!r}: entry @{m.entry} must reference an `op` or `btype` node",
                    m.span,
                )
            anchor_node = ci.source

    return ResolvedMatcher(
        decl=m,
        captures=captures,
        anchor_capture=anchor_capture,
        anchor_node=anchor_node,
    )


def _is_recursive_set(file: ast_.File) -> set[str]:
    """Helper: names of rules that self-reference (direct or via cycle)."""
    out: set[str] = set()
    name_to_rule = {r.name: r for r in file.rules}

    def collect_refs(p: ast_.PatternExpr, refs: set[str]) -> None:
        if isinstance(p, ast_.RuleRef):
            refs.add(p.name)
        elif isinstance(p, ast_.Maybe):
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

    for r in file.rules:
        refs: set[str] = set()
        collect_refs(r.pattern, refs)
        if r.name in refs:
            out.add(r.name)

    return out
