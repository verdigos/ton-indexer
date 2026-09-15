from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum


class Severity(Enum):
    ERROR = "error"
    WARNING = "warning"


# Diagnostic codes are plain strings passed at each call site (R0xx resolver,
# P0xx parser, L0xx lexer, C0xx/B0xx compiler). Build-language codes are named
# here so there is one discoverable record of them.
R010_UNRESOLVED_REFERENCE = "R010_UNRESOLVED_REFERENCE"
"""A bare identifier in a build-statement expression does not resolve to a
pattern capture or a previously declared `let` name."""

R011_PARSE_TARGET_NOT_CAPTURE = "R011_PARSE_TARGET_NOT_CAPTURE"
"""The target of a `parse X as T` statement is not a pattern capture."""

R012_UNKNOWN_MESSAGE_TYPE = "R012_UNKNOWN_MESSAGE_TYPE"
"""A message type named in `parse … as T` is not present in the host's
message-type registry."""

R013_DUP_LET = "R013_DUP_LET"
"""A `let` name collides with a pattern capture or an earlier `let`."""

R014_DUP_OUT_FIELD = "R014_DUP_OUT_FIELD"
"""The same field name appears more than once in an `out` block."""

R016_BUILD_NO_OUT = "R016_BUILD_NO_OUT"
"""A matcher has no `out` block or `produces switch`, so there is nothing to produce."""

R017_MULTIPLE_OUT = "R017_MULTIPLE_OUT"
"""More than one `out` block in a matcher body; exactly one is allowed."""

R022_UNNORMALIZED_PRODUCES_CLASS = "R022_UNNORMALIZED_PRODUCES_CLASS"
"""A PascalCase (class-name) `produces` token has no class->btype entry in
mch/btypes.py CLASS_TO_BTYPE, so it cannot be normalized to the block's real
btype. Left unresolved it would silently emit the class name itself as the
produced btype, which the serializer and
legacy blocks never key on. Add the entry, or use the bare btype string."""

R023_PARSE_TARGET_NOT_BASE_BLOCK = "R023_PARSE_TARGET_NOT_BASE_BLOCK"
"""A `parse X as T` statement targets a capture bound by a `btype` node: a
composed block built by an earlier matcher. A produced block's `get_body()`
reads `event_nodes[0]`, whose identity after the
prior matcher's `merge_blocks` is not the anchor protocol message, so the
re-parse does not round-trip and `.body` silently null-propagates. Read the
prior matcher's fields via `X.data.<field>` instead of re-parsing."""

R024_NESTED_COMPREHENSION = "R024_NESTED_COMPREHENSION"
"""A `map|any|all(xs as e => body)` comprehension appears inside another
comprehension (either the outer `xs` or `body`). Nested comprehensions are
forbidden: exactly one bound element var per comprehension, no
comprehension in the iterated list or the body."""

# A user declaration repeats a built-in opcode with its built-in value.
R025_BUILTIN_OPCODE_REDECLARED = "R025_BUILTIN_OPCODE_REDECLARED"

# A user declaration gives a built-in opcode a different value.
R026_BUILTIN_OPCODE_MISMATCH = "R026_BUILTIN_OPCODE_MISMATCH"

# Two opcode-set members resolve to the same unsigned 32-bit value.
R027_DUPLICATE_OPCODE_SET_MEMBER = "R027_DUPLICATE_OPCODE_SET_MEMBER"

# A capture has conflicting parser lists. Rule-body annotations contribute to
# every matcher referencing the rule; each matcher has one list per capture.
R028_PARSE_ANNOTATION_CONFLICT = "R028_PARSE_ANNOTATION_CONFLICT"

# A capture has both a parse annotation and a written parse statement.
R029_PARSE_ANNOTATION_AND_STATEMENT = "R029_PARSE_ANNOTATION_AND_STATEMENT"

# Only op nodes support parse annotations; composed bodies do not round-trip.
R030_PARSE_ANNOTATION_ON_COMPOSED = "R030_PARSE_ANNOTATION_ON_COMPOSED"

# Function bodies reference only parameters in value positions. Substitution is
# call-by-name: bind expensive or faulting arguments with `let` before calling.
R031_FN_BODY_FREE_NAME = "R031_FN_BODY_FREE_NAME"
R032_FN_BODY_LOOKUP = "R032_FN_BODY_LOOKUP"
R033_FN_BODY_COMPREHENSION = "R033_FN_BODY_COMPREHENSION"
R034_DUP_FN = "R034_DUP_FN"
R035_FN_NAME_COLLISION = "R035_FN_NAME_COLLISION"
R036_FN_ARITY = "R036_FN_ARITY"
R037_FN_CYCLE = "R037_FN_CYCLE"

R038_IMPORT_NOT_FOUND = "R038_IMPORT_NOT_FOUND"
R039_IMPORT_CYCLE = "R039_IMPORT_CYCLE"
R040_DUPLICATE_IMPORT = "R040_DUPLICATE_IMPORT"
R041_IMPORT_NAME_COLLISION = "R041_IMPORT_NAME_COLLISION"
R042_MANIFEST_ORDER = "R042_MANIFEST_ORDER"

P011_DUPLICATE_DIRECTIVE = "P011_DUPLICATE_DIRECTIVE"
"""A matcher declares the same at-most-once directive more than once
(currently `shape`)."""

# Membership requires a name or a field path on the left.
P013_MEMBERSHIP_LEFT_OPERAND = "P013_MEMBERSHIP_LEFT_OPERAND"

# Membership lists contain only literal scalars.
P014_MEMBERSHIP_MEMBER_NOT_LITERAL = "P014_MEMBERSHIP_MEMBER_NOT_LITERAL"

# Strict parsing is only allowed in expression position.
P015_STRICT_PARSE_STATEMENT = "P015_STRICT_PARSE_STATEMENT"

# Opcode sets require at least one member and cannot carry a predicate.
P016_EMPTY_OPCODE_SET = "P016_EMPTY_OPCODE_SET"
P017_WHERE_ON_OPCODE_SET = "P017_WHERE_ON_OPCODE_SET"

P018_DUPLICATE_RELATION_CLAUSE = "P018_DUPLICATE_RELATION_CLAUSE"
P019_ARROW_AFTER_RELATION_BLOCK = "P019_ARROW_AFTER_RELATION_BLOCK"

# A node needs a capture before its parse annotation.
P020_PARSE_ANNOTATION_NEEDS_CAPTURE = "P020_PARSE_ANNOTATION_NEEDS_CAPTURE"

# An or/xor block needs at least two arms.
P021_CHOICE_NEEDS_TWO_ARMS = "P021_CHOICE_NEEDS_TWO_ARMS"

P022_ALTERNATIVE_OPERATOR_REMOVED = "P022_ALTERNATIVE_OPERATOR_REMOVED"
P023_BARE_PARSE_EXPRESSION = "P023_BARE_PARSE_EXPRESSION"
P024_MODIFIER_ON_CHILDREN_SET = "P024_MODIFIER_ON_CHILDREN_SET"
P025_CALLEE_NOT_NAME = "P025_CALLEE_NOT_NAME"
P026_IMPORT_AFTER_DECLARATION = "P026_IMPORT_AFTER_DECLARATION"
P027_EXPORT_NOT_ALLOWED = "P027_EXPORT_NOT_ALLOWED"
P028_MANY_CAP = "P028_MANY_CAP"

C011_UNKNOWN_SHAPER = "C011_UNKNOWN_SHAPER"
"""The `shape` directive names a shaper that is not present in the host's
shaper registry."""

C012_CYCLIC_RULE_SHAPE = "C012_CYCLIC_RULE_SHAPE"
"""A `rule NAME cyclic = …` body violates the cyclic-descent shape rules:
captures inside the body, a self-reference that is
not the last atom of its chain, a `maybe`- or `peek`-wrapped self-reference, or a
reference to another recursive rule."""

C014_PEEK_SUBTREE_CONSUMES = "C014_PEEK_SUBTREE_CONSUMES"
"""A `peek` node reaches another pattern node that is not itself `peek`, so
the subtree would still consume blocks or absorb auxiliary siblings."""

C015_WHERE_EXPR_CAPTURE_NOT_ENTRY = "C015_WHERE_EXPR_CAPTURE_NOT_ENTRY"
"""A capture name used in `where (expr)` is not the matcher's entry capture."""

C016_ENTRY_RELATIONS_IN_CHAIN = "C016_ENTRY_RELATIONS_IN_CHAIN"
C017_RECURSIVE_RULE_XOR = "C017_RECURSIVE_RULE_XOR"
C017_MANY = "C017_MANY"


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
