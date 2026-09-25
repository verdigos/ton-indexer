from __future__ import annotations

from dataclasses import dataclass, field

R023_PARSE_TARGET_NOT_BASE_BLOCK = "R023_PARSE_TARGET_NOT_BASE_BLOCK"
"""A `parse X as T` statement targets a capture bound by a `btype` node: a
composed block built by an earlier matcher. A produced block's `get_body()`
reads `event_nodes[0]`, whose identity after the
prior matcher's `merge_blocks` is not the anchor protocol message, so the
re-parse does not round-trip and `.body` silently null-propagates. Read the
prior matcher's fields via `X.data.<field>` instead of re-parsing."""

R024_NESTED_COMPREHENSION = "R024_NESTED_COMPREHENSION"
"""A `map|any|all|find|stable_sort_by(xs as e => body)` comprehension appears
inside another comprehension (either the outer `xs` or `body`). Nested
comprehensions are forbidden: exactly one bound element var per comprehension, no
comprehension in the iterated list or the body."""

# A capture has conflicting parser lists. Rule-body annotations contribute to
# every matcher referencing the rule; each matcher has one list per capture.
R028_PARSE_ANNOTATION_CONFLICT = "R028_PARSE_ANNOTATION_CONFLICT"

# Only op nodes support parse annotations; composed bodies do not round-trip.
R030_PARSE_ANNOTATION_ON_COMPOSED = "R030_PARSE_ANNOTATION_ON_COMPOSED"

# Function bodies reference only parameters in value positions. Substitution is
# call-by-name: bind expensive or faulting arguments with `let` before calling.
R031_FN_BODY_FREE_NAME = "R031_FN_BODY_FREE_NAME"
R033_FN_BODY_COMPREHENSION = "R033_FN_BODY_COMPREHENSION"
R035_FN_NAME_COLLISION = "R035_FN_NAME_COLLISION"
R036_FN_ARITY = "R036_FN_ARITY"
R041_IMPORT_NAME_COLLISION = "R041_IMPORT_NAME_COLLISION"
R042_MANIFEST_ORDER = "R042_MANIFEST_ORDER"
R043_UNKNOWN_BTYPE = "R043_UNKNOWN_BTYPE"
R044_UNDECLARED_PREDICATE = "R044_UNDECLARED_PREDICATE"


@dataclass(frozen=True)
class Span:
    path: str
    start_line: int
    start_col: int
    end_line: int
    end_col: int
    start_off: int
    end_off: int


@dataclass(frozen=True)
class Diagnostic:
    code: str
    message: str
    span: Span
    suggestion: str | None = None


@dataclass
class DiagnosticBag:
    items: list[Diagnostic] = field(default_factory=list)

    def error(self, code: str, message: str, span: Span, suggestion: str | None = None) -> None:
        self.items.append(Diagnostic(code, message, span, suggestion))


class CompileError(Exception):
    def __init__(self, bag: DiagnosticBag) -> None:
        self.bag = bag
        super().__init__(f"compilation failed with {len(bag.items)} diagnostics")


def format_diagnostic(d: Diagnostic, source_text: str) -> str:
    lines = source_text.splitlines()
    idx = d.span.start_line - 1
    source_line = lines[idx] if 0 <= idx < len(lines) else ""
    span_len = max(1, d.span.end_col - d.span.start_col)
    caret_indent = " " * (d.span.start_col - 1)
    caret = caret_indent + "^" * span_len
    header = (
        f"{d.span.path}:{d.span.start_line}:{d.span.start_col}: "
        f"error: {d.message} ({d.code})"
    )
    parts = [header, source_line, caret]
    if d.suggestion:
        parts.append(d.suggestion)
    return "\n".join(parts)
