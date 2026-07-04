from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Union

from indexer.events.mch.diagnostics import Span


class Edge(Enum):
    CHILD = "->"
    PARENT = "<-"


# ---------------------------------------------------------------------------
# Expression language (M4 §9.6). Parsed and carried through the frontend; the
# v0 compiler ignores these nodes entirely (no evaluation, no lowering).
# ---------------------------------------------------------------------------


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
class FieldRef(Expr):
    """Leading-dot field access `.field` — the current block's data field
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
    """`lookup KIND(args)` — interface-repository query (§9.2)."""
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
class Node(PatternExpr):
    head: NodeHead
    capture: str | None
    where_predicate: str | None
    span: Span
    # Inline `where (expr)` form (§4 M4). Mutually exclusive with the named
    # `where_predicate` form; the compiler ignores it in v0.
    where_expr: Expr | None = None


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


@dataclass(frozen=True)
class OpcodeDecl:
    name: str
    value: int
    span: Span


@dataclass(frozen=True)
class PredicateDecl:
    name: str
    span: Span


@dataclass(frozen=True)
class RuleDecl:
    name: str
    pattern: PatternExpr
    span: Span
    # Recursion strategy for a self-referential rule (spec §10.3): "frontier"
    # (default, T18 frontier walk) or "cyclic" (cyclic descent — self-refs at
    # chain ends lower to back-edges in the matcher graph). Parsed from the
    # contextual modifier `rule NAME cyclic = …`; ignored for non-recursive
    # rules.
    strategy: str = "frontier"


# ---------------------------------------------------------------------------
# Build statements (M4 §9). Appear in a matcher body after the pattern.
# ---------------------------------------------------------------------------


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

    @property
    def msg_type(self) -> str:
        """First (or only) declared message type. Kept for single-type callers."""
        return self.msg_types[0]


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
class BrokenStmt(BuildStmt):
    """`broken when expr`."""
    condition: Expr
    span: Span


@dataclass(frozen=True)
class FlowStmt(BuildStmt):
    """`flow jetton(owner, master, amount)` | `flow ton(account, amount)` |
    `flow fees(account, amount)` — value-flow effects on the produced block
    (Block.value_flow add_jetton/add_ton/add_fees, spec §9.8)."""
    kind: str
    args: tuple[Expr, ...]
    span: Span


@dataclass(frozen=True)
class OutField:
    name: str
    value: Expr
    span: Span


@dataclass(frozen=True)
class OutStmt(BuildStmt):
    """`out { field: expr <sep> ... }`."""
    fields: tuple[OutField, ...]
    span: Span


@dataclass(frozen=True)
class MatcherDecl:
    name: str
    produces: tuple[str, ...]
    # None when the matcher is declarative (build statements with an `out`
    # block instead of a registered-builder reference, §9.5).
    build: str | None
    entry: str | None
    include_excess: bool
    include_bounces: bool
    pattern: PatternExpr
    span: Span
    # M4 build statements, in source order. Empty for M1-M3 matchers.
    build_stmts: tuple[BuildStmt, ...] = ()
    # `shape IDENT` directive (M2R R06, §8.7): registered shaper invoked by the
    # engine after merge, for post-build tree surgery on the produced block's
    # region. None when absent.
    shape: str | None = None

    @property
    def produces_primary(self) -> str:
        """First declared `produces` name. Convenience accessor for code that
        needs a single type (e.g. `CompiledMatcher.produces_cls_name`); the
        union (spec §8.1 M2) is documentation + registry validation, and
        selecting among the declared types at build time is the builder's job."""
        return self.produces[0]


@dataclass(frozen=True)
class CapturePath:
    """A.B.C as parsed in `stitch.key`."""
    root_capture: str
    attrs: tuple[str, ...]
    span: Span


@dataclass(frozen=True)
class StitchDecl:
    name: str
    partial: str
    final: str
    key: CapturePath
    ttl_value: int
    ttl_unit: str
    merge: str
    span: Span


@dataclass(frozen=True)
class File:
    path: str
    opcodes:    tuple[OpcodeDecl, ...]
    predicates: tuple[PredicateDecl, ...]
    rules:      tuple[RuleDecl, ...]
    matchers:   tuple[MatcherDecl, ...]
    stitches:   tuple[StitchDecl, ...]
