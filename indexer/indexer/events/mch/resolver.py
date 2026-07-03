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
class _CaptureOccurrence:
    """One @name position in a matcher pattern, with enough context to decide
    whether several positions sharing a name are mutually exclusive (spec §6.2)."""
    node: ast_.Node
    under_maybe: bool
    under_recursive: bool
    # One entry per enclosing EXCLUSIVE alternative: (alt_id, branch_idx, branch_count).
    branch_path: tuple[tuple[int, int, int], ...]
    # True when any enclosing alternative is NOT exclusive (plain `|` failing the
    # ExclusiveOr-upgrade heuristic). Such positions can never anchor (spec §8.3).
    under_nonexclusive_alt: bool = False


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
        if not isinstance(b, ast_.Node) or b.where_predicate is not None:
            return False
        head = b.head
        if isinstance(head, ast_.OpHead):
            ref = head.ref
            op = ref if isinstance(ref, int) else (
                opcodes.get(ref) if opcodes.get(ref) is not None else registries.opcodes.get(ref)
            )
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
    """True if a and b sit in different branches of some common exclusive alternative."""
    b_by_alt = {alt_id: idx for alt_id, idx, _n in b.branch_path}
    for alt_id, idx, _n in a.branch_path:
        if alt_id in b_by_alt and b_by_alt[alt_id] != idx:
            return True
    return False


def _merged_optional(occs: list[_CaptureOccurrence]) -> bool:
    """Optionality of a capture defined in several mutually exclusive positions.

    The capture always binds (optional=False) only when no occurrence is under
    `maybe` and some exclusive alternative common to ALL occurrences is fully
    covered — one occurrence in each of its branches. Otherwise a branch without
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
    """Validate an `entry` capture that touches alternative branches (spec §8.3 M2).

    Legal only as an opcode-set anchor: every occurrence is itself a branch of
    one exclusive alternative, one per branch, all branches covered, each branch
    a bare concrete `op`/`btype` node without `where`. Returns (group, "") on
    success or (None, reason) for the R007 message.
    """
    if any(o.under_recursive for o in occs):
        return None, "is inside a recursive rule"
    if any(o.under_maybe for o in occs):
        return None, "is inside a `maybe`-wrapped subtree"
    if any(o.under_nonexclusive_alt for o in occs):
        return None, (
            "sits in a non-exclusive alternative branch; an opcode-set anchor "
            "requires an exclusive alternative (`^`, or `|` of distinct bare "
            "concrete heads) (spec §8.3 M2)"
        )
    if any(len(o.branch_path) != 1 for o in occs):
        return None, (
            "must sit directly on the branches of a single exclusive "
            "alternative, not nested in or outside one (spec §8.3 M2)"
        )
    alt_id = occs[0].branch_path[0][0]
    if any(o.branch_path[0][0] != alt_id for o in occs):
        return None, (
            "occurrences span different alternatives; an opcode-set anchor "
            "requires a single exclusive alternative (spec §8.3 M2)"
        )
    alt = alt_nodes[alt_id]
    idxs = {o.branch_path[0][1] for o in occs}
    if len(occs) != len(alt.branches) or idxs != set(range(len(alt.branches))):
        return None, (
            "must be bound on every branch of the exclusive alternative "
            "(spec §8.3 M2)"
        )
    by_idx = {o.branch_path[0][1]: o for o in occs}
    for idx, branch in enumerate(alt.branches):
        occ = by_idx[idx]
        if branch is not occ.node:
            return None, (
                "requires every branch of the exclusive alternative to be a "
                "bare node carrying the capture (no chains or nesting) "
                "(spec §8.3 M2)"
            )
        if not isinstance(occ.node.head, (ast_.OpHead, ast_.BTypeHead)):
            return None, (
                "requires concrete `op`/`btype` heads on every branch of the "
                "exclusive alternative (spec §8.3 M2)"
            )
        if occ.node.where_predicate is not None:
            return None, (
                "does not support a `where` clause on opcode-set anchor "
                "branches; split the matcher or check in the builder"
            )
    return alt, ""


@dataclass
class ResolvedMatcher:
    decl: ast_.MatcherDecl
    captures: dict[str, CaptureInfo]
    anchor_capture: str
    anchor_node: ast_.Node
    # Opcode-set anchor (spec §8.3 M2): when `entry` names a capture bound on
    # every branch of one exclusive alternative of bare concrete heads, this is
    # that Alternative; `anchor_node` then holds a representative branch node.
    anchor_group: ast_.Alternative | None = None


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
    _check_produces_union(file, registries, bag)
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


def _check_produces_union(file: ast_.File, registries: Registries, bag: DiagnosticBag) -> None:
    """Validate every name in a matcher's `produces` union (spec §8.1 M2) against
    the host's block-type registry. A single-name `produces` is a 1-element union."""
    for m in file.matchers:
        for name in m.produces:
            if name not in registries.block_types:
                bag.error(
                    "C003_UNKNOWN_BLOCK_TYPE",
                    f"matcher {m.name!r}: produces type {name!r} is not registered",
                    m.span,
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
    # Walk the pattern; collect capture occurrences; validate op/predicate refs.
    # Occurrences are merged into CaptureInfo afterwards so that the same name in
    # mutually exclusive alternative branches is legal (spec §6.2).
    occurrences: dict[str, list[_CaptureOccurrence]] = {}
    alt_ids: dict[int, int] = {}  # id(Alternative) -> stable sequential id
    alt_nodes: dict[int, ast_.Alternative] = {}  # stable id -> Alternative node

    def visit(
        p: ast_.PatternExpr,
        under_maybe: bool,
        under_recursive: bool,
        branch_path: tuple[tuple[int, int, int], ...],
        under_nonexclusive_alt: bool = False,
    ) -> None:
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
                occurrences.setdefault(p.capture, []).append(_CaptureOccurrence(
                    node=p,
                    under_maybe=under_maybe,
                    under_recursive=under_recursive,
                    branch_path=branch_path,
                    under_nonexclusive_alt=under_nonexclusive_alt,
                ))
        elif isinstance(p, ast_.Maybe):
            visit(p.inner, True, under_recursive, branch_path, under_nonexclusive_alt)
        elif isinstance(p, ast_.Sequence):
            visit(p.head, under_maybe, under_recursive, branch_path, under_nonexclusive_alt)
            for _e, atom in p.tail:
                visit(atom, under_maybe, under_recursive, branch_path, under_nonexclusive_alt)
        elif isinstance(p, ast_.Alternative):
            exclusive = _alternative_is_exclusive(p, opcodes, registries)
            if exclusive:
                alt_id = alt_ids.setdefault(id(p), len(alt_ids))
                alt_nodes[alt_id] = p
            for idx, b in enumerate(p.branches):
                if exclusive:
                    visit(b, under_maybe, under_recursive,
                          branch_path + ((alt_id, idx, len(p.branches)),),
                          under_nonexclusive_alt)
                else:
                    visit(b, under_maybe, under_recursive, branch_path, True)
        elif isinstance(p, ast_.ChildrenBlock):
            for it in p.items:
                visit(it, under_maybe, under_recursive, branch_path, under_nonexclusive_alt)
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
                    visit(target.pattern, under_maybe, under_recursive=False,
                          branch_path=branch_path, under_nonexclusive_alt=under_nonexclusive_alt)
                else:
                    visit(target.pattern, under_maybe, under_recursive=True,
                          branch_path=branch_path, under_nonexclusive_alt=under_nonexclusive_alt)

    visit(m.pattern, under_maybe=False, under_recursive=False, branch_path=())

    captures: dict[str, CaptureInfo] = {}
    for cap_name, occs in occurrences.items():
        if len(occs) > 1:
            # Legal only when every pair of occurrences is separated by an
            # exclusive alternative AND all agree on recursion context.
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
                    f"(occurrences are not in mutually exclusive alternative branches)",
                    offending.span,
                )
                first = occs[0]
                captures[cap_name] = CaptureInfo(
                    name=cap_name,
                    head=first.node.head,
                    optional=first.under_maybe,
                    list_binding=first.under_recursive,
                    source=first.node,
                )
                continue
            captures[cap_name] = CaptureInfo(
                name=cap_name,
                head=occs[0].node.head,
                optional=_merged_optional(occs),
                list_binding=occs[0].under_recursive,
                source=occs[0].node,
            )
        else:
            o = occs[0]
            captures[cap_name] = CaptureInfo(
                name=cap_name,
                head=o.node.head,
                optional=o.under_maybe,
                list_binding=o.under_recursive,
                source=o.node,
            )

    # Anchor.
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
                # opcode-set anchor over one exclusive alternative (spec §8.3 M2).
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
        anchor_group=anchor_group,
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
