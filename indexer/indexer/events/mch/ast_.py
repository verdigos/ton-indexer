from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Union

from indexer.events.mch.diagnostics import Span


class Edge(Enum):
    CHILD = "->"
    PARENT = "<-"


# Expression nodes parsed and carried through the frontend. The
# v0 compiler ignores these nodes entirely (no evaluation, no lowering).


class Expr:
    pass


@dataclass(frozen=True)
class IntLit(Expr):
    value: int
    span: Span


@dataclass(frozen=True)
class StrLit(Expr):
    value: str  # decoded string contents (without surrounding quotes)
    span: Span


@dataclass(frozen=True)
class BoolLit(Expr):
    value: bool
    span: Span


@dataclass(frozen=True)
class NullLit(Expr):
    span: Span


@dataclass(frozen=True)
class NameRef(Expr):
    """A bare identifier used as a value: a capture name or a `let` name."""
    name: str
    span: Span


@dataclass(frozen=True)
class DotSelf(Expr):
    """The candidate block inside an inline `where (expr)` clause."""
    span: Span


@dataclass(frozen=True)
class FieldRef(Expr):
    """Leading-dot field access `.field`, the current block's data field
    (valid only as a primary inside an inline `where (expr)` clause)."""
    field: str
    span: Span


@dataclass(frozen=True)
class FieldAccess(Expr):
    target: Expr
    field: str
    span: Span


@dataclass(frozen=True)
class Call(Expr):
    callee: Expr
    args: tuple[Expr, ...]
    span: Span


@dataclass(frozen=True)
class LookupExpr(Expr):
    """`lookup KIND(args)` — interface-repository query."""
    kind: str
    args: tuple[Expr, ...]
    span: Span


@dataclass(frozen=True)
class UnaryOp(Expr):
    op: str  # "not" | "-"
    operand: Expr
    span: Span


@dataclass(frozen=True)
class BinaryOp(Expr):
    op: str  # "or" "and" "==" "!=" "<" "<=" ">" ">=" "??"
    left: Expr
    right: Expr
    span: Span


@dataclass(frozen=True)
class Ternary(Expr):
    """Python-style conditional: `then if cond else orelse`."""
    cond: Expr
    then: Expr
    orelse: Expr
    span: Span


@dataclass(frozen=True)
class ListLit(Expr):
    """`[expr, ...]` is a list value whose elements evaluate in order."""
    elements: tuple[Expr, ...]
    span: Span


@dataclass(frozen=True)
class ParseExpr(Expr):
    """Parse a Block, Cell, or base64-BOC string against message types.

    `parse!` faults on an invalid target or total failure; `parse?` sets
    `nullable` and returns null for those failures instead."""
    target: Expr
    msg_types: tuple[str, ...]
    span: Span
    nullable: bool = False


@dataclass(frozen=True)
class Comprehension(Expr):
    """`map|any|all|find|stable_sort_by(xs as e => body)` lambda comprehension.

    `kind` is "map", "any", "all", "find", or "stable_sort_by". `var` is
    the element binder, visible ONLY inside `body` (a new scope frame). No nested
    comprehensions. Distinct from the `map(xs,"f")` builtin call, which the
    parser tells apart by the `as` keyword."""
    kind: str  # "map" | "any" | "all" | "find" | "stable_sort_by"
    xs: Expr
    var: str
    body: Expr
    span: Span


@dataclass(frozen=True)
class RecordField:
    name: str
    value: Expr
    span: Span


@dataclass(frozen=True)
class RecordLit(Expr):
    """`{key: expr, ...}` is a record value with string keys."""
    fields: tuple[RecordField, ...]
    span: Span


class NodeHead:
    pass


@dataclass(frozen=True)
class OpHead(NodeHead):
    """Concrete opcode: either a registered identifier (str) or literal int."""
    ref: Union[str, int]
    span: Span


@dataclass(frozen=True)
class BTypeHead(NodeHead):
    name: str
    span: Span


@dataclass(frozen=True)
class PredHead(NodeHead):
    name: str
    span: Span


@dataclass(frozen=True)
class AnyHead(NodeHead):
    span: Span


class PatternExpr:
    pass


@dataclass(frozen=True)
class Relations:
    children_all: PatternExpr | None
    children_first: tuple[PatternExpr, ...] | None
    parent: PatternExpr | None
    span: Span


@dataclass(frozen=True)
class Node(PatternExpr):
    head: NodeHead
    capture: str | None
    where_predicate: str | None
    span: Span
    # Inline `where (expr)` form. Mutually exclusive with the named
    # `where_predicate` form; the compiler ignores it in v0.
    where_expr: Expr | None = None
    relations: Relations | None = None
    parse_types: tuple[str, ...] | None = None


@dataclass(frozen=True)
class RuleRef(PatternExpr):
    name: str
    span: Span


@dataclass(frozen=True)
class ChildrenBlock(PatternExpr):
    items: tuple[PatternExpr, ...]
    span: Span


@dataclass(frozen=True)
class Maybe(PatternExpr):
    inner: PatternExpr
    span: Span


@dataclass(frozen=True)
class Peek(PatternExpr):
    inner: PatternExpr
    span: Span


@dataclass(frozen=True)
class Many(PatternExpr):
    inner: PatternExpr
    min: int
    max: int | None
    span: Span


@dataclass(frozen=True)
class Sequence(PatternExpr):
    """One-or-more atoms connected by edges.

    A bare atom is Sequence(head=atom, tail=()). A linear chain `A -> B -> C` is
    Sequence(head=A, tail=((Edge.CHILD, B), (Edge.CHILD, C))).
    """
    head: PatternExpr
    tail: tuple[tuple[Edge, PatternExpr], ...]
    span: Span


@dataclass(frozen=True)
class Alternative(PatternExpr):
    branches: tuple[PatternExpr, ...]
    span: Span
    exclusive: bool = False
    opcode_set: bool = False


def children(p: PatternExpr) -> tuple[PatternExpr, ...]:
    if isinstance(p, Node):
        if p.relations is None:
            return ()
        relations = p.relations
        return tuple(
            child
            for child in (
                relations.children_all,
                *(relations.children_first or ()),
                relations.parent,
            )
            if child is not None
        )
    if isinstance(p, (Maybe, Peek, Many)):
        return (p.inner,)
    if isinstance(p, Sequence):
        return (p.head, *(child for _edge, child in p.tail))
    if isinstance(p, Alternative):
        return p.branches
    if isinstance(p, ChildrenBlock):
        return p.items
    return ()


@dataclass(frozen=True)
class OpcodeDecl:
    name: str
    value: int
    span: Span
    exported: bool = False


@dataclass(frozen=True)
class PredicateDecl:
    name: str
    span: Span
    exported: bool = False


@dataclass(frozen=True)
class FnDecl:
    name: str
    params: tuple[str, ...]
    body: Expr
    span: Span
    exported: bool = False


@dataclass(frozen=True)
class RuleDecl:
    name: str
    pattern: PatternExpr
    span: Span
    # Recursion strategy for a self-referential rule: "frontier"
    # (default) or "cyclic" (cyclic descent, with self-references at
    # chain ends lower to back-edges in the matcher graph). Parsed from the
    # contextual modifier `rule NAME cyclic = …`; ignored for non-recursive
    # rules.
    strategy: str = "frontier"
    exported: bool = False


# Build statements appear in a matcher body after the pattern.


class BuildStmt:
    pass


@dataclass(frozen=True)
class ParseStmt(BuildStmt):
    """`parse CAPTURE as MessageType (| MessageType)*`.

    Alternatives are tried in source order; the first type whose constructor
    parses the body wins (soft — all failing yields a null body).
    """
    capture: str
    msg_types: tuple[str, ...]
    span: Span


@dataclass(frozen=True)
class LetStmt(BuildStmt):
    """`let NAME = expr`."""
    name: str
    value: Expr
    span: Span


@dataclass(frozen=True)
class RejectStmt(BuildStmt):
    """`reject when expr`."""
    condition: Expr
    span: Span


@dataclass(frozen=True)
class FailedStmt(BuildStmt):
    """`failed when expr`."""
    condition: Expr
    span: Span


@dataclass(frozen=True)
class OutField:
    name: str
    value: Expr
    span: Span
    # `name?: expr` — omit this key from the produced data map when the value
    # evaluates to null. Default fields always emit the key.
    optional: bool = False


@dataclass(frozen=True)
class OutStmt(BuildStmt):
    """`out { field: expr <sep> ... }`."""
    fields: tuple[OutField, ...]
    span: Span


@dataclass(frozen=True)
class SwitchBranch:
    """One arm of a `produces switch`. `condition is None` marks the `else`
    arm. `btype` is the produced block type this arm selects; `out` its fields."""
    condition: Expr | None
    btype: str
    out: OutStmt
    span: Span


@dataclass(frozen=True)
class SwitchStmt(BuildStmt):
    """`produces switch { when cond => btype out {...} ... else => btype out {...} }`.
    Branches are tested in order; the first true (or `else`) wins,
    selecting both the produced btype and its `out` fields."""
    branches: tuple[SwitchBranch, ...]
    span: Span


@dataclass(frozen=True)
class MatcherDecl:
    name: str
    produces: tuple[str, ...]
    include_excess: bool
    include_bounces: bool
    pattern: PatternExpr
    span: Span
    # Build statements in source order.
    build_stmts: tuple[BuildStmt, ...] = ()
    # `shape IDENT` names a registered shaper invoked by the
    # engine after merge, for post-build tree surgery on the produced block's
    # region. None when absent.
    shape: str | None = None
    # `action direct` makes this matcher's output the action row.
    direct_action: bool = False


@dataclass(frozen=True)
class ImportDecl:
    path: str
    span: Span


@dataclass(frozen=True)
class File:
    path: str
    opcodes:    tuple[OpcodeDecl, ...]
    predicates: tuple[PredicateDecl, ...]
    rules:      tuple[RuleDecl, ...]
    matchers:   tuple[MatcherDecl, ...]
    fns:        tuple[FnDecl, ...] = ()
    imports:    tuple[ImportDecl, ...] = ()
