from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum


class Severity(Enum):
    ERROR = "error"
    WARNING = "warning"


# Diagnostic codes are plain strings passed at each call site (R0xx resolver,
# P0xx parser, L0xx lexer, C0xx/B0xx compiler). New codes introduced for the M4
# build language are named here so there is one discoverable record of them.
R010_UNRESOLVED_REFERENCE = "R010_UNRESOLVED_REFERENCE"
"""A bare identifier in a build-statement expression does not resolve to a
pattern capture or a previously declared `let` name (§9)."""

R011_PARSE_TARGET_NOT_CAPTURE = "R011_PARSE_TARGET_NOT_CAPTURE"
"""The target of a `parse X as T` statement is not a pattern capture (§9.1)."""

R012_UNKNOWN_MESSAGE_TYPE = "R012_UNKNOWN_MESSAGE_TYPE"
"""A message type named in `parse … as T` is not present in the host's
message-type registry (§9.1)."""

R013_DUP_LET = "R013_DUP_LET"
"""A `let` name collides with a pattern capture or an earlier `let` (§9.2)."""

R014_DUP_OUT_FIELD = "R014_DUP_OUT_FIELD"
"""The same field name appears more than once in an `out` block (§9.5)."""

R015_BUILD_AMBIGUOUS = "R015_BUILD_AMBIGUOUS"
"""A matcher declares both a `build` directive (registered builder) and build
statements — ambiguous; use exactly one form (§9)."""

R016_BUILD_NO_OUT = "R016_BUILD_NO_OUT"
"""A declarative matcher (no `build` directive) has build statements but no
`out` block — nothing to produce (§9.5)."""

R017_MULTIPLE_OUT = "R017_MULTIPLE_OUT"
"""More than one `out` block in a matcher body; exactly one is allowed (§9.5)."""

R018_BAD_FLOW = "R018_BAD_FLOW"
"""A `flow` statement names an unknown kind or has the wrong number of
arguments — must be jetton(3), ton(2), or fees(2) (§9.8)."""

P011_DUPLICATE_DIRECTIVE = "P011_DUPLICATE_DIRECTIVE"
"""A matcher declares the same at-most-once directive more than once
(currently: `shape`, §8.7)."""

C011_UNKNOWN_SHAPER = "C011_UNKNOWN_SHAPER"
"""The `shape` directive names a shaper that is not present in the host's
shaper registry (§8.7, §11.1)."""

C012_CYCLIC_RULE_SHAPE = "C012_CYCLIC_RULE_SHAPE"
"""A `rule NAME cyclic = …` body violates the cyclic-descent shape rules
(§10.3, [M2R, R11/T28]): captures inside the body, a self-reference that is
not the last atom of its chain, a `maybe`-wrapped self-reference, or a
reference to another recursive rule."""


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
    severity: Severity
    code: str
    message: str
    span: Span
    suggestion: str | None = None


@dataclass
class DiagnosticBag:
    items: list[Diagnostic] = field(default_factory=list)

    def error(self, code: str, message: str, span: Span, suggestion: str | None = None) -> None:
        self.items.append(Diagnostic(Severity.ERROR, code, message, span, suggestion))

    def warning(self, code: str, message: str, span: Span, suggestion: str | None = None) -> None:
        self.items.append(Diagnostic(Severity.WARNING, code, message, span, suggestion))

    @property
    def has_errors(self) -> bool:
        return any(d.severity is Severity.ERROR for d in self.items)


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
        f"{d.severity.value}: {d.message} ({d.code})"
    )
    parts = [header, source_line, caret]
    if d.suggestion:
        parts.append(d.suggestion)
    return "\n".join(parts)
