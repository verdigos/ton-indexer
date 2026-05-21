from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Union

from indexer.events.mch.diagnostics import Span


class Edge(Enum):
    CHILD = "->"
    PARENT = "<-"


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


@dataclass(frozen=True)
class MatcherDecl:
    name: str
    produces: str
    build: str
    entry: str | None
    include_excess: bool
    include_bounces: bool
    pattern: PatternExpr
    span: Span


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
