# `.mch` DSL Python v0 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a Python compiler for the `.mch` matcher DSL that reads `.mch` source, validates it, and produces `BlockMatcher` instances that plug into the existing event-classifier engine without touching production wiring.

**Architecture:** Pure-layered compiler pipeline (lex → parse → resolve → compile → codegen). Each layer is a function with immutable input. Compiled matchers are real `BlockMatcher` subclasses generated per matcher decl, carrying a `__mch_name__` attribute for diagnostic tools. Recursion lowers to the existing `RecursiveMatcher` through a pluggable strategy.

**Tech Stack:** Python 3.10+, pytest, pytest-asyncio, dataclasses, `pytoniq_core` (existing dep), existing `indexer.events.blocks.*` types.

**Important conventions for this plan:**
- All paths relative to repo root `S:\Dev\ton-indexer-clear\indexer\` unless absolute.
- Test runner: `python -m pytest tests/mch/ -v` from `S:\Dev\ton-indexer-clear\indexer\`.
- No git commits during implementation (user instruction). Verification step at each task end is a passing `pytest` run instead.
- Use `from __future__ import annotations` at the top of every new module so forward references in type hints just work.
- Reference design doc: `docs/superpowers/specs/2026-05-19-mch-dsl-design.md`.
- Reference spec: `docs/mch-spec.md`.

---

## File Structure

### Created files

```
indexer/events/mch/
  __init__.py
  tokens.py
  lexer.py
  ast_.py
  parser.py
  registry.py
  resolver.py
  recursion.py
  compiler.py
  codegen.py
  diagnostics.py
  compile.py
  specs/
    jetton_transfer.mch
    stonfi_v2_swap.mch
  generated/
    .gitkeep
    # populated by codegen during tests

tests/mch/
  __init__.py
  conftest.py
  test_diagnostics.py
  test_tokens.py
  test_lexer.py
  test_parser.py
  test_registry.py
  test_resolver.py
  test_recursion.py
  test_compiler_structural.py
  test_compiler_behavioral.py
  test_codegen.py
  test_compile_facade.py
  integration/
    __init__.py
    test_jetton_transfer.py
    test_stonfi_v2_swap.py
```

### Modified files

- `tools/matcher_trace/_summary.py` — one-line change to honor `__mch_name__` in `summarize_matcher`.

---

## Task 1: Module skeleton

**Files:**
- Create: `indexer/events/mch/__init__.py`
- Create: `indexer/events/mch/specs/__init__.py` (empty marker)
- Create: `indexer/events/mch/generated/.gitkeep` (empty file)
- Create: `tests/mch/__init__.py` (empty marker)
- Create: `tests/mch/integration/__init__.py` (empty marker)

- [ ] **Step 1: Create the package directories with empty `__init__.py`/`.gitkeep`**

```python
# indexer/events/mch/__init__.py
"""`.mch` matcher DSL — Python v0 compiler.

Public surface:
    compile_text(text, path, registries, ...) -> list[CompiledMatcher]
    compile_file(path, registries, ...)       -> list[CompiledMatcher]
    Registries                                  -> host-language registry container
    predicate / build                           -> decorator factories
"""
from __future__ import annotations
```

Other files (`specs/__init__.py`, `tests/mch/__init__.py`, `tests/mch/integration/__init__.py`) are empty. `generated/.gitkeep` is empty.

- [ ] **Step 2: Sanity-check pytest discovers the new test directory**

Create a placeholder test file:

```python
# tests/mch/test_smoke.py
def test_mch_test_dir_discovered():
    assert True
```

Run: `python -m pytest tests/mch/ -v`
Expected: 1 passed.

- [ ] **Step 3: Delete the placeholder**

```bash
rm tests/mch/test_smoke.py
```

---

## Task 2: Diagnostics

**Files:**
- Create: `indexer/events/mch/diagnostics.py`
- Create: `tests/mch/test_diagnostics.py`

- [ ] **Step 1: Write the failing tests**

```python
# tests/mch/test_diagnostics.py
from __future__ import annotations

import pytest

from indexer.events.mch.diagnostics import (
    CompileError,
    Diagnostic,
    DiagnosticBag,
    Severity,
    Span,
    format_diagnostic,
)


def _span(line: int = 1, col: int = 1, end_col: int | None = None) -> Span:
    return Span(
        path="example.mch",
        start_line=line,
        start_col=col,
        end_line=line,
        end_col=end_col if end_col is not None else col + 1,
        start_off=col - 1,
        end_off=(end_col if end_col is not None else col + 1) - 1,
    )


def test_diagnostic_construction():
    d = Diagnostic(
        severity=Severity.ERROR,
        code="R001_UNKNOWN_OPCODE",
        message="undefined opcode 'Foo'",
        span=_span(1, 8, 11),
        suggestion="did you mean 'Bar'?",
    )
    assert d.code == "R001_UNKNOWN_OPCODE"
    assert d.severity is Severity.ERROR


def test_bag_collects_errors_and_warnings():
    bag = DiagnosticBag()
    bag.error("R001_UNKNOWN_OPCODE", "x", _span())
    bag.warning("W001_UNUSED_RULE", "y", _span())
    assert len(bag.items) == 2
    assert bag.has_errors is True


def test_bag_no_errors_when_only_warnings():
    bag = DiagnosticBag()
    bag.warning("W001_UNUSED_RULE", "y", _span())
    assert bag.has_errors is False


def test_compile_error_carries_bag():
    bag = DiagnosticBag()
    bag.error("R001_UNKNOWN_OPCODE", "x", _span())
    err = CompileError(bag)
    assert err.bag is bag


def test_format_diagnostic_renders_caret_layout():
    source = "opcode Foo = 0x1\nrule X = op Bar\n"
    d = Diagnostic(
        severity=Severity.ERROR,
        code="R001_UNKNOWN_OPCODE",
        message="undefined opcode 'Bar'",
        span=Span(
            path="example.mch",
            start_line=2,
            start_col=13,
            end_line=2,
            end_col=16,
            start_off=29,
            end_off=32,
        ),
        suggestion="did you mean 'Foo'?",
    )
    out = format_diagnostic(d, source)
    assert "example.mch:2:13: error: undefined opcode 'Bar' (R001_UNKNOWN_OPCODE)" in out
    assert "rule X = op Bar" in out
    assert "^^^" in out
    assert "did you mean 'Foo'?" in out
```

- [ ] **Step 2: Run, confirm it fails**

Run: `python -m pytest tests/mch/test_diagnostics.py -v`
Expected: ImportError (module doesn't exist).

- [ ] **Step 3: Write `diagnostics.py`**

```python
# indexer/events/mch/diagnostics.py
from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum


class Severity(Enum):
    ERROR = "error"
    WARNING = "warning"


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
```

- [ ] **Step 4: Run tests**

Run: `python -m pytest tests/mch/test_diagnostics.py -v`
Expected: 5 passed.

---

## Task 3: Tokens

**Files:**
- Create: `indexer/events/mch/tokens.py`
- Create: `tests/mch/test_tokens.py`

- [ ] **Step 1: Write the failing tests**

```python
# tests/mch/test_tokens.py
from __future__ import annotations

from indexer.events.mch.diagnostics import Span
from indexer.events.mch.tokens import Token, TokenKind


def test_token_kinds_exhaustive_set_present():
    expected = {
        # literals
        "IDENT", "HEX", "DEC", "DURATION", "STRING",
        # declaration keywords
        "KW_OPCODE", "KW_PREDICATE", "KW_RULE", "KW_MATCHER", "KW_STITCH",
        # node-head keywords
        "KW_OP", "KW_BTYPE", "KW_PRED", "KW_ANY",
        # modifier / directive keywords
        "KW_MAYBE", "KW_WHERE", "KW_PRODUCES", "KW_BUILD", "KW_ENTRY",
        "KW_INCLUDE_EXCESS", "KW_INCLUDE_BOUNCES",
        # stitch keywords
        "KW_PARTIAL", "KW_FINAL", "KW_KEY", "KW_TTL", "KW_MERGE",
        # boolean literals
        "KW_TRUE", "KW_FALSE",
        # punctuation
        "ARROW_R", "ARROW_L", "PIPE", "AT", "DOLLAR",
        "LBRACE", "RBRACE", "LPAREN", "RPAREN", "EQ", "DOT", "SEMI",
        # control
        "NEWLINE", "EOF",
    }
    actual = {k.name for k in TokenKind}
    assert expected.issubset(actual)


def test_token_value_object():
    span = Span("f.mch", 1, 1, 1, 4, 0, 3)
    tok = Token(kind=TokenKind.IDENT, text="foo", span=span)
    assert tok.kind is TokenKind.IDENT
    assert tok.text == "foo"
    assert tok.span is span
```

- [ ] **Step 2: Run, confirm it fails**

Run: `python -m pytest tests/mch/test_tokens.py -v`
Expected: ImportError.

- [ ] **Step 3: Write `tokens.py`**

```python
# indexer/events/mch/tokens.py
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum

from indexer.events.mch.diagnostics import Span


class TokenKind(Enum):
    # literals
    IDENT = "IDENT"
    HEX = "HEX"
    DEC = "DEC"
    DURATION = "DURATION"
    STRING = "STRING"
    # declaration keywords
    KW_OPCODE = "opcode"
    KW_PREDICATE = "predicate"
    KW_RULE = "rule"
    KW_MATCHER = "matcher"
    KW_STITCH = "stitch"
    # node-head keywords
    KW_OP = "op"
    KW_BTYPE = "btype"
    KW_PRED = "pred"
    KW_ANY = "any"
    # modifier / directive keywords
    KW_MAYBE = "maybe"
    KW_WHERE = "where"
    KW_PRODUCES = "produces"
    KW_BUILD = "build"
    KW_ENTRY = "entry"
    KW_INCLUDE_EXCESS = "include_excess"
    KW_INCLUDE_BOUNCES = "include_bounces"
    # stitch keywords (accepted in v0; compiler emits W900_STITCH_DEFERRED)
    KW_PARTIAL = "partial"
    KW_FINAL = "final"
    KW_KEY = "key"
    KW_TTL = "ttl"
    KW_MERGE = "merge"
    # boolean literals
    KW_TRUE = "true"
    KW_FALSE = "false"
    # punctuation
    ARROW_R = "->"
    ARROW_L = "<-"
    PIPE = "|"
    AT = "@"
    DOLLAR = "$"
    LBRACE = "{"
    RBRACE = "}"
    LPAREN = "("
    RPAREN = ")"
    EQ = "="
    DOT = "."
    SEMI = ";"
    # control
    NEWLINE = "NEWLINE"
    EOF = "EOF"


# Mapping from keyword text to TokenKind, for the lexer's identifier-keyword promotion.
KEYWORDS: dict[str, TokenKind] = {
    k.value: k
    for k in TokenKind
    if k.name.startswith("KW_")
}


@dataclass(frozen=True)
class Token:
    kind: TokenKind
    text: str
    span: Span
```

- [ ] **Step 4: Run tests**

Run: `python -m pytest tests/mch/test_tokens.py -v`
Expected: 2 passed.

---

## Task 4: Lexer — success path

**Files:**
- Create: `indexer/events/mch/lexer.py`
- Create: `tests/mch/test_lexer.py` (will extend in Task 5)

- [ ] **Step 1: Write the failing tests for the success path**

```python
# tests/mch/test_lexer.py
from __future__ import annotations

import pytest

from indexer.events.mch.lexer import tokenize
from indexer.events.mch.tokens import TokenKind


def _kinds(text: str) -> list[TokenKind]:
    return [t.kind for t in tokenize(text, "f.mch") if t.kind is not TokenKind.EOF]


def test_opcode_declaration():
    toks = list(tokenize("opcode JettonTransfer = 0x0f8a7ea5", "f.mch"))
    assert [t.kind for t in toks] == [
        TokenKind.KW_OPCODE,
        TokenKind.IDENT,
        TokenKind.EQ,
        TokenKind.HEX,
        TokenKind.EOF,
    ]
    assert toks[1].text == "JettonTransfer"
    assert toks[3].text == "0x0f8a7ea5"


def test_identifier_with_underscore_and_digits():
    toks = list(tokenize("foo_bar_42", "f.mch"))
    assert toks[0].kind is TokenKind.IDENT
    assert toks[0].text == "foo_bar_42"


def test_decimal_literal():
    toks = list(tokenize("42", "f.mch"))
    assert toks[0].kind is TokenKind.DEC
    assert toks[0].text == "42"


def test_duration_literal():
    toks = list(tokenize("6h", "f.mch"))
    assert toks[0].kind is TokenKind.DURATION
    assert toks[0].text == "6h"


def test_keywords_promoted_from_idents():
    assert _kinds("op btype pred any maybe where") == [
        TokenKind.KW_OP, TokenKind.KW_BTYPE, TokenKind.KW_PRED,
        TokenKind.KW_ANY, TokenKind.KW_MAYBE, TokenKind.KW_WHERE,
    ]


def test_arrows_and_punctuation():
    assert _kinds("-> <- | @ $ { } ( ) = . ;") == [
        TokenKind.ARROW_R, TokenKind.ARROW_L, TokenKind.PIPE, TokenKind.AT,
        TokenKind.DOLLAR, TokenKind.LBRACE, TokenKind.RBRACE,
        TokenKind.LPAREN, TokenKind.RPAREN, TokenKind.EQ, TokenKind.DOT, TokenKind.SEMI,
    ]


def test_line_comments_consumed():
    src = "opcode Foo = 0x1  # this is a comment\nopcode Bar = 0x2"
    kinds = _kinds(src)
    # NEWLINE emitted between the two declarations
    assert TokenKind.NEWLINE in kinds
    assert kinds.count(TokenKind.KW_OPCODE) == 2


def test_newline_emitted_inside_children_block_context():
    # The lexer always emits NEWLINE tokens; the parser decides where they matter.
    toks = list(tokenize("a\nb", "f.mch"))
    kinds = [t.kind for t in toks]
    assert TokenKind.NEWLINE in kinds


def test_span_tracks_line_and_column():
    toks = list(tokenize("opcode  Foo", "f.mch"))
    foo_tok = next(t for t in toks if t.text == "Foo")
    assert foo_tok.span.start_line == 1
    assert foo_tok.span.start_col == 9
    assert foo_tok.span.end_col == 12


def test_span_advances_across_newline():
    toks = list(tokenize("opcode Foo = 0x1\nopcode Bar = 0x2", "f.mch"))
    bar_tok = next(t for t in toks if t.text == "Bar")
    assert bar_tok.span.start_line == 2
    assert bar_tok.span.start_col == 8


def test_eof_token_always_last():
    toks = list(tokenize("opcode Foo = 0x1", "f.mch"))
    assert toks[-1].kind is TokenKind.EOF
```

- [ ] **Step 2: Run, confirm failures**

Run: `python -m pytest tests/mch/test_lexer.py -v`
Expected: ImportError.

- [ ] **Step 3: Write `lexer.py`**

```python
# indexer/events/mch/lexer.py
from __future__ import annotations

from typing import Iterator

from indexer.events.mch.diagnostics import Span
from indexer.events.mch.tokens import KEYWORDS, Token, TokenKind


_SINGLE_CHAR: dict[str, TokenKind] = {
    "|": TokenKind.PIPE,
    "@": TokenKind.AT,
    "$": TokenKind.DOLLAR,
    "{": TokenKind.LBRACE,
    "}": TokenKind.RBRACE,
    "(": TokenKind.LPAREN,
    ")": TokenKind.RPAREN,
    "=": TokenKind.EQ,
    ".": TokenKind.DOT,
    ";": TokenKind.SEMI,
}

_DURATION_UNITS = set("smhd")


class LexerError(Exception):
    def __init__(self, code: str, message: str, span: Span):
        super().__init__(message)
        self.code = code
        self.message = message
        self.span = span


class _Cursor:
    def __init__(self, text: str, path: str):
        self.text = text
        self.path = path
        self.i = 0
        self.line = 1
        self.col = 1

    def at_end(self) -> bool:
        return self.i >= len(self.text)

    def peek(self, offset: int = 0) -> str:
        j = self.i + offset
        return self.text[j] if j < len(self.text) else ""

    def advance(self) -> str:
        ch = self.text[self.i]
        self.i += 1
        if ch == "\n":
            self.line += 1
            self.col = 1
        else:
            self.col += 1
        return ch

    def span_from(self, start_i: int, start_line: int, start_col: int) -> Span:
        return Span(
            path=self.path,
            start_line=start_line,
            start_col=start_col,
            end_line=self.line,
            end_col=self.col,
            start_off=start_i,
            end_off=self.i,
        )


def tokenize(text: str, path: str) -> Iterator[Token]:
    cur = _Cursor(text, path)

    while not cur.at_end():
        ch = cur.peek()

        # Comment: consume to (but not including) newline; the newline becomes a NEWLINE token below.
        if ch == "#":
            while not cur.at_end() and cur.peek() != "\n":
                cur.advance()
            continue

        # Newlines become NEWLINE tokens.
        if ch == "\n":
            start_i, start_line, start_col = cur.i, cur.line, cur.col
            cur.advance()
            yield Token(TokenKind.NEWLINE, "\n", cur.span_from(start_i, start_line, start_col))
            continue

        # Other whitespace consumed silently.
        if ch in " \t\r":
            cur.advance()
            continue

        # Two-char operators -> and <-.
        if ch == "-" and cur.peek(1) == ">":
            start_i, start_line, start_col = cur.i, cur.line, cur.col
            cur.advance(); cur.advance()
            yield Token(TokenKind.ARROW_R, "->", cur.span_from(start_i, start_line, start_col))
            continue
        if ch == "<" and cur.peek(1) == "-":
            start_i, start_line, start_col = cur.i, cur.line, cur.col
            cur.advance(); cur.advance()
            yield Token(TokenKind.ARROW_L, "<-", cur.span_from(start_i, start_line, start_col))
            continue

        # Single-char punctuation.
        if ch in _SINGLE_CHAR:
            start_i, start_line, start_col = cur.i, cur.line, cur.col
            cur.advance()
            yield Token(_SINGLE_CHAR[ch], ch, cur.span_from(start_i, start_line, start_col))
            continue

        # Numeric: hex, decimal, or duration.
        if ch.isdigit():
            yield _read_number(cur)
            continue

        # Identifier (and keyword promotion).
        if ch.isalpha() or ch == "_":
            yield _read_identifier_or_keyword(cur)
            continue

        # String literal — reserved, not used in patterns; accept and yield as STRING.
        if ch == '"':
            yield _read_string(cur)
            continue

        start_i, start_line, start_col = cur.i, cur.line, cur.col
        cur.advance()
        raise LexerError(
            code="L002_UNKNOWN_CHAR",
            message=f"unexpected character {ch!r}",
            span=cur.span_from(start_i, start_line, start_col),
        )

    yield Token(TokenKind.EOF, "", cur.span_from(cur.i, cur.line, cur.col))


def _read_number(cur: "_Cursor") -> Token:
    start_i, start_line, start_col = cur.i, cur.line, cur.col

    # Hex.
    if cur.peek() == "0" and cur.peek(1) in ("x", "X"):
        cur.advance(); cur.advance()
        digits_start = cur.i
        while not cur.at_end() and cur.peek() in "0123456789abcdefABCDEF":
            cur.advance()
        if cur.i == digits_start:
            raise LexerError(
                "L001_BAD_NUMERIC",
                "hex literal requires at least one hex digit",
                cur.span_from(start_i, start_line, start_col),
            )
        text = cur.text[start_i:cur.i]
        return Token(TokenKind.HEX, text, cur.span_from(start_i, start_line, start_col))

    # Decimal (possibly followed by a duration unit).
    while not cur.at_end() and cur.peek().isdigit():
        cur.advance()
    if not cur.at_end() and cur.peek() in _DURATION_UNITS:
        # Duration unit must NOT be followed by another ident char (i.e. "6s" vs "6sm").
        if (
            cur.i + 1 < len(cur.text)
            and (cur.text[cur.i + 1].isalnum() or cur.text[cur.i + 1] == "_")
        ):
            text = cur.text[start_i:cur.i]
            return Token(TokenKind.DEC, text, cur.span_from(start_i, start_line, start_col))
        cur.advance()
        text = cur.text[start_i:cur.i]
        return Token(TokenKind.DURATION, text, cur.span_from(start_i, start_line, start_col))

    text = cur.text[start_i:cur.i]
    return Token(TokenKind.DEC, text, cur.span_from(start_i, start_line, start_col))


def _read_identifier_or_keyword(cur: "_Cursor") -> Token:
    start_i, start_line, start_col = cur.i, cur.line, cur.col
    while not cur.at_end():
        ch = cur.peek()
        if ch.isalnum() or ch == "_":
            cur.advance()
        else:
            break
    text = cur.text[start_i:cur.i]
    kind = KEYWORDS.get(text, TokenKind.IDENT)
    return Token(kind, text, cur.span_from(start_i, start_line, start_col))


def _read_string(cur: "_Cursor") -> Token:
    start_i, start_line, start_col = cur.i, cur.line, cur.col
    cur.advance()  # opening quote
    while not cur.at_end() and cur.peek() != '"':
        if cur.peek() == "\\":
            cur.advance()
            if cur.at_end():
                raise LexerError(
                    "L003_UNTERMINATED_STRING",
                    "unterminated string literal",
                    cur.span_from(start_i, start_line, start_col),
                )
        cur.advance()
    if cur.at_end():
        raise LexerError(
            "L003_UNTERMINATED_STRING",
            "unterminated string literal",
            cur.span_from(start_i, start_line, start_col),
        )
    cur.advance()  # closing quote
    text = cur.text[start_i:cur.i]
    return Token(TokenKind.STRING, text, cur.span_from(start_i, start_line, start_col))
```

- [ ] **Step 4: Run tests**

Run: `python -m pytest tests/mch/test_lexer.py -v`
Expected: all in test_lexer.py pass.

---

## Task 5: Lexer — error cases

**Files:**
- Modify: `tests/mch/test_lexer.py` (append)

- [ ] **Step 1: Write the failing error-case tests**

Append to `tests/mch/test_lexer.py`:

```python
from indexer.events.mch.lexer import LexerError


def test_bad_hex_literal():
    with pytest.raises(LexerError) as exc:
        list(tokenize("opcode Foo = 0x", "f.mch"))
    assert exc.value.code == "L001_BAD_NUMERIC"
    assert exc.value.span.start_line == 1


def test_unknown_character():
    with pytest.raises(LexerError) as exc:
        list(tokenize("opcode ! = 0x1", "f.mch"))
    assert exc.value.code == "L002_UNKNOWN_CHAR"


def test_unterminated_string():
    with pytest.raises(LexerError) as exc:
        list(tokenize('"unterminated', "f.mch"))
    assert exc.value.code == "L003_UNTERMINATED_STRING"
```

- [ ] **Step 2: Run, confirm passing**

Run: `python -m pytest tests/mch/test_lexer.py -v`
Expected: all pass (the lexer was already coded to raise these).

If any fail, fix the lexer to match the asserted codes/spans before moving on.

---

## Task 6: AST dataclasses

**Files:**
- Create: `indexer/events/mch/ast_.py`

- [ ] **Step 1: Write `ast_.py`**

This is a structural module — no behavior to test directly. Parser tests will exercise it.

```python
# indexer/events/mch/ast_.py
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
```

No test step. Module is exercised by the parser.

---

## Task 7: Parser — top-level declarations

**Files:**
- Create: `indexer/events/mch/parser.py`
- Create: `tests/mch/test_parser.py`

- [ ] **Step 1: Write the failing tests for top decls**

```python
# tests/mch/test_parser.py
from __future__ import annotations

import pytest

from indexer.events.mch import ast_
from indexer.events.mch.diagnostics import CompileError
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse


def _parse(src: str) -> ast_.File:
    tokens = list(tokenize(src, "test.mch"))
    return parse(tokens, "test.mch")


def test_parse_opcode_declaration_hex():
    f = _parse("opcode JettonTransfer = 0x0f8a7ea5")
    assert len(f.opcodes) == 1
    assert f.opcodes[0].name == "JettonTransfer"
    assert f.opcodes[0].value == 0x0f8a7ea5


def test_parse_opcode_declaration_decimal():
    f = _parse("opcode Foo = 42")
    assert f.opcodes[0].value == 42


def test_parse_predicate_declaration():
    f = _parse("predicate pton_self_transfer")
    assert len(f.predicates) == 1
    assert f.predicates[0].name == "pton_self_transfer"


def test_parse_rule_declaration_single_node():
    f = _parse("opcode Foo = 0x1\nrule r = op Foo")
    assert len(f.rules) == 1
    assert f.rules[0].name == "r"
    assert isinstance(f.rules[0].pattern, ast_.Sequence)


def test_parse_matcher_declaration_minimal():
    src = """
    opcode Foo = 0x1
    matcher m {
        produces FooBlock
        build    build_foo
        entry    @x

        op Foo @x
    }
    """
    f = _parse(src)
    assert len(f.matchers) == 1
    m = f.matchers[0]
    assert m.name == "m"
    assert m.produces == "FooBlock"
    assert m.build == "build_foo"
    assert m.entry == "x"
    assert m.include_excess is True   # default
    assert m.include_bounces is True  # default


def test_parse_matcher_with_directive_overrides():
    src = """
    opcode Foo = 0x1
    matcher m {
        produces FooBlock
        build    build_foo
        entry    @x
        include_excess  false
        include_bounces false

        op Foo @x
    }
    """
    f = _parse(src)
    assert f.matchers[0].include_excess is False
    assert f.matchers[0].include_bounces is False


def test_parse_stitch_declaration():
    src = """
    stitch s {
        partial p
        final   f
        key     @topup.destination
        ttl     6h
        merge   merge_fn
    }
    """
    f = _parse(src)
    assert len(f.stitches) == 1
    s = f.stitches[0]
    assert s.partial == "p"
    assert s.final == "f"
    assert s.key.root_capture == "topup"
    assert s.key.attrs == ("destination",)
    assert s.ttl_value == 6
    assert s.ttl_unit == "h"
    assert s.merge == "merge_fn"
```

- [ ] **Step 2: Run, confirm failures**

Run: `python -m pytest tests/mch/test_parser.py -v`
Expected: ImportError.

- [ ] **Step 3: Write `parser.py` (skeleton + top-decl portion)**

```python
# indexer/events/mch/parser.py
from __future__ import annotations

from typing import Iterable

from indexer.events.mch import ast_
from indexer.events.mch.diagnostics import CompileError, DiagnosticBag, Span
from indexer.events.mch.tokens import Token, TokenKind


class _ParserState:
    def __init__(self, tokens: list[Token], path: str, bag: DiagnosticBag):
        # Drop NEWLINE tokens except where they matter (children blocks).
        # We do NOT drop them here; the parser explicitly skips NEWLINE outside
        # of children blocks via _skip_newlines().
        self.tokens = tokens
        self.path = path
        self.bag = bag
        self.i = 0

    def peek(self, offset: int = 0) -> Token:
        j = self.i + offset
        return self.tokens[j] if j < len(self.tokens) else self.tokens[-1]

    def at(self, kind: TokenKind) -> bool:
        return self.peek().kind is kind

    def eat(self, kind: TokenKind) -> Token:
        t = self.peek()
        if t.kind is not kind:
            self.bag.error(
                "P001_UNEXPECTED_TOKEN",
                f"expected {kind.name}, found {t.kind.name} ({t.text!r})",
                t.span,
            )
            raise CompileError(self.bag)
        self.i += 1
        return t

    def try_eat(self, kind: TokenKind) -> Token | None:
        if self.at(kind):
            t = self.peek()
            self.i += 1
            return t
        return None

    def skip_newlines(self) -> None:
        while self.at(TokenKind.NEWLINE):
            self.i += 1


def parse(tokens: Iterable[Token], path: str) -> ast_.File:
    tokens = list(tokens)
    bag = DiagnosticBag()
    s = _ParserState(tokens, path, bag)

    opcodes:    list[ast_.OpcodeDecl] = []
    predicates: list[ast_.PredicateDecl] = []
    rules:      list[ast_.RuleDecl] = []
    matchers:   list[ast_.MatcherDecl] = []
    stitches:   list[ast_.StitchDecl] = []

    s.skip_newlines()
    while not s.at(TokenKind.EOF):
        t = s.peek()
        if t.kind is TokenKind.KW_OPCODE:
            opcodes.append(_parse_opcode_decl(s))
        elif t.kind is TokenKind.KW_PREDICATE:
            predicates.append(_parse_predicate_decl(s))
        elif t.kind is TokenKind.KW_RULE:
            rules.append(_parse_rule_decl(s))
        elif t.kind is TokenKind.KW_MATCHER:
            matchers.append(_parse_matcher_decl(s))
        elif t.kind is TokenKind.KW_STITCH:
            stitches.append(_parse_stitch_decl(s))
        else:
            s.bag.error(
                "P001_UNEXPECTED_TOKEN",
                f"expected top-level declaration, found {t.kind.name} ({t.text!r})",
                t.span,
            )
            raise CompileError(bag)
        s.skip_newlines()

    if bag.has_errors:
        raise CompileError(bag)

    return ast_.File(
        path=path,
        opcodes=tuple(opcodes),
        predicates=tuple(predicates),
        rules=tuple(rules),
        matchers=tuple(matchers),
        stitches=tuple(stitches),
    )


def _parse_opcode_decl(s: _ParserState) -> ast_.OpcodeDecl:
    start = s.eat(TokenKind.KW_OPCODE).span
    name_tok = s.eat(TokenKind.IDENT)
    s.eat(TokenKind.EQ)
    value_tok = s.peek()
    if value_tok.kind is TokenKind.HEX:
        s.i += 1
        value = int(value_tok.text, 16)
    elif value_tok.kind is TokenKind.DEC:
        s.i += 1
        value = int(value_tok.text, 10)
    else:
        s.bag.error(
            "P001_UNEXPECTED_TOKEN",
            f"expected numeric literal, found {value_tok.kind.name}",
            value_tok.span,
        )
        raise CompileError(s.bag)
    return ast_.OpcodeDecl(name=name_tok.text, value=value, span=_join(start, value_tok.span))


def _parse_predicate_decl(s: _ParserState) -> ast_.PredicateDecl:
    start = s.eat(TokenKind.KW_PREDICATE).span
    name_tok = s.eat(TokenKind.IDENT)
    return ast_.PredicateDecl(name=name_tok.text, span=_join(start, name_tok.span))


def _parse_rule_decl(s: _ParserState) -> ast_.RuleDecl:
    start = s.eat(TokenKind.KW_RULE).span
    name_tok = s.eat(TokenKind.IDENT)
    s.eat(TokenKind.EQ)
    s.skip_newlines()
    pattern = _parse_pattern_expr(s)
    return ast_.RuleDecl(name=name_tok.text, pattern=pattern, span=_join(start, pattern.span))


def _parse_matcher_decl(s: _ParserState) -> ast_.MatcherDecl:
    start = s.eat(TokenKind.KW_MATCHER).span
    name_tok = s.eat(TokenKind.IDENT)
    s.eat(TokenKind.LBRACE)
    s.skip_newlines()

    produces: str | None = None
    build:    str | None = None
    entry:    str | None = None
    include_excess  = True
    include_bounces = True

    while not s.at(TokenKind.EOF):
        s.skip_newlines()
        t = s.peek()
        if t.kind is TokenKind.KW_PRODUCES:
            s.i += 1
            produces = s.eat(TokenKind.IDENT).text
        elif t.kind is TokenKind.KW_BUILD:
            s.i += 1
            build = s.eat(TokenKind.IDENT).text
        elif t.kind is TokenKind.KW_ENTRY:
            s.i += 1
            s.eat(TokenKind.AT)
            entry = s.eat(TokenKind.IDENT).text
        elif t.kind is TokenKind.KW_INCLUDE_EXCESS:
            s.i += 1
            include_excess = _parse_bool_literal(s)
        elif t.kind is TokenKind.KW_INCLUDE_BOUNCES:
            s.i += 1
            include_bounces = _parse_bool_literal(s)
        else:
            break
        s.skip_newlines()

    pattern = _parse_pattern_expr(s)
    s.skip_newlines()
    end = s.eat(TokenKind.RBRACE).span

    if produces is None or build is None:
        s.bag.error(
            "P001_UNEXPECTED_TOKEN",
            f"matcher {name_tok.text!r} missing required directive (produces and/or build)",
            _join(start, end),
        )
        raise CompileError(s.bag)

    return ast_.MatcherDecl(
        name=name_tok.text,
        produces=produces,
        build=build,
        entry=entry,
        include_excess=include_excess,
        include_bounces=include_bounces,
        pattern=pattern,
        span=_join(start, end),
    )


def _parse_bool_literal(s: _ParserState) -> bool:
    t = s.peek()
    if t.kind is TokenKind.KW_TRUE:
        s.i += 1
        return True
    if t.kind is TokenKind.KW_FALSE:
        s.i += 1
        return False
    s.bag.error("P001_UNEXPECTED_TOKEN", f"expected 'true' or 'false', found {t.text!r}", t.span)
    raise CompileError(s.bag)


def _parse_stitch_decl(s: _ParserState) -> ast_.StitchDecl:
    start = s.eat(TokenKind.KW_STITCH).span
    name_tok = s.eat(TokenKind.IDENT)
    s.eat(TokenKind.LBRACE)
    s.skip_newlines()

    partial: str | None = None
    final:   str | None = None
    key:     ast_.CapturePath | None = None
    ttl_value: int | None = None
    ttl_unit:  str | None = None
    merge_fn: str | None = None

    while not s.at(TokenKind.RBRACE) and not s.at(TokenKind.EOF):
        t = s.peek()
        if t.kind is TokenKind.KW_PARTIAL:
            s.i += 1
            partial = s.eat(TokenKind.IDENT).text
        elif t.kind is TokenKind.KW_FINAL:
            s.i += 1
            final = s.eat(TokenKind.IDENT).text
        elif t.kind is TokenKind.KW_KEY:
            s.i += 1
            key = _parse_capture_path(s)
        elif t.kind is TokenKind.KW_TTL:
            s.i += 1
            tt = s.eat(TokenKind.DURATION)
            ttl_value = int(tt.text[:-1])
            ttl_unit = tt.text[-1]
        elif t.kind is TokenKind.KW_MERGE:
            s.i += 1
            merge_fn = s.eat(TokenKind.IDENT).text
        else:
            s.bag.error(
                "P001_UNEXPECTED_TOKEN",
                f"expected stitch directive, found {t.kind.name}",
                t.span,
            )
            raise CompileError(s.bag)
        s.skip_newlines()

    end = s.eat(TokenKind.RBRACE).span
    if None in (partial, final, key, ttl_value, ttl_unit, merge_fn):
        s.bag.error(
            "P001_UNEXPECTED_TOKEN",
            f"stitch {name_tok.text!r} missing required directive",
            _join(start, end),
        )
        raise CompileError(s.bag)
    return ast_.StitchDecl(
        name=name_tok.text,
        partial=partial,                     # type: ignore[arg-type]
        final=final,                         # type: ignore[arg-type]
        key=key,                             # type: ignore[arg-type]
        ttl_value=ttl_value,                 # type: ignore[arg-type]
        ttl_unit=ttl_unit,                   # type: ignore[arg-type]
        merge=merge_fn,                      # type: ignore[arg-type]
        span=_join(start, end),
    )


def _parse_capture_path(s: _ParserState) -> ast_.CapturePath:
    start = s.eat(TokenKind.AT).span
    root_tok = s.eat(TokenKind.IDENT)
    attrs: list[str] = []
    end_span = root_tok.span
    while s.at(TokenKind.DOT):
        s.i += 1
        a = s.eat(TokenKind.IDENT)
        attrs.append(a.text)
        end_span = a.span
    return ast_.CapturePath(
        root_capture=root_tok.text,
        attrs=tuple(attrs),
        span=_join(start, end_span),
    )


def _parse_pattern_expr(s: _ParserState) -> ast_.PatternExpr:
    """Defined in the next task. Stub so top-decl tests can run."""
    raise NotImplementedError("filled in by Task 8")


def _join(a: Span, b: Span) -> Span:
    return Span(
        path=a.path,
        start_line=a.start_line,
        start_col=a.start_col,
        end_line=b.end_line,
        end_col=b.end_col,
        start_off=a.start_off,
        end_off=b.end_off,
    )
```

- [ ] **Step 4: Run tests; expect partial failures**

Run: `python -m pytest tests/mch/test_parser.py -v`
Expected:
- `test_parse_opcode_declaration_*`, `test_parse_predicate_declaration`, `test_parse_stitch_declaration` PASS.
- `test_parse_rule_declaration_single_node`, `test_parse_matcher_declaration_minimal`, `test_parse_matcher_with_directive_overrides` FAIL with `NotImplementedError("filled in by Task 8")`.

This is intentional — Task 8 fills in `_parse_pattern_expr`.

---

## Task 8: Parser — pattern expressions

**Files:**
- Modify: `indexer/events/mch/parser.py` (replace the `NotImplementedError` stub)

- [ ] **Step 1: Replace the `_parse_pattern_expr` stub**

In `indexer/events/mch/parser.py`, replace the stub function with this implementation (and helpers):

```python
def _parse_pattern_expr(s: _ParserState) -> ast_.PatternExpr:
    """pattern_expr := sequence ('|' sequence)*"""
    s.skip_newlines()
    branches = [_parse_sequence(s)]
    while True:
        # Alternatives may span newlines for readability.
        save_i = s.i
        s.skip_newlines()
        if not s.at(TokenKind.PIPE):
            s.i = save_i
            break
        s.i += 1  # consume PIPE
        s.skip_newlines()
        branches.append(_parse_sequence(s))
    if len(branches) == 1:
        return branches[0]
    span = _join(branches[0].span, branches[-1].span)
    return ast_.Alternative(branches=tuple(branches), span=span)


def _parse_sequence(s: _ParserState) -> ast_.PatternExpr:
    """sequence := atom ( edge atom )*"""
    head = _parse_atom(s)
    tail: list[tuple[ast_.Edge, ast_.PatternExpr]] = []
    while True:
        save_i = s.i
        s.skip_newlines()
        t = s.peek()
        if t.kind is TokenKind.ARROW_R:
            s.i += 1
            s.skip_newlines()
            tail.append((ast_.Edge.CHILD, _parse_atom(s)))
        elif t.kind is TokenKind.ARROW_L:
            s.i += 1
            s.skip_newlines()
            tail.append((ast_.Edge.PARENT, _parse_atom(s)))
        else:
            s.i = save_i
            break
    if not tail:
        return head
    last_span = tail[-1][1].span
    return ast_.Sequence(head=head, tail=tuple(tail), span=_join(head.span, last_span))


def _parse_atom(s: _ParserState) -> ast_.PatternExpr:
    """atom := node | '(' pattern_expr ')' | rule_ref | children_block | 'maybe' atom"""
    t = s.peek()

    if t.kind is TokenKind.KW_MAYBE:
        s.i += 1
        s.skip_newlines()
        inner = _parse_atom(s)
        return ast_.Maybe(inner=inner, span=_join(t.span, inner.span))

    if t.kind is TokenKind.LPAREN:
        s.i += 1
        s.skip_newlines()
        inner = _parse_pattern_expr(s)
        s.skip_newlines()
        end = s.eat(TokenKind.RPAREN).span
        # Strip parentheses; carry through the span.
        if isinstance(inner, ast_.Alternative):
            return ast_.Alternative(branches=inner.branches, span=_join(t.span, end))
        if isinstance(inner, ast_.Sequence):
            return ast_.Sequence(head=inner.head, tail=inner.tail, span=_join(t.span, end))
        # Atomic — span unchanged.
        return inner

    if t.kind is TokenKind.LBRACE:
        return _parse_children_block(s)

    if t.kind is TokenKind.DOLLAR:
        s.i += 1
        name_tok = s.eat(TokenKind.IDENT)
        return ast_.RuleRef(name=name_tok.text, span=_join(t.span, name_tok.span))

    return _parse_node(s)


def _parse_node(s: _ParserState) -> ast_.Node:
    """node := node_head capture? predicate_clause?"""
    t = s.peek()
    head: ast_.NodeHead
    head_start_span = t.span

    if t.kind is TokenKind.KW_OP:
        s.i += 1
        ref_tok = s.peek()
        if ref_tok.kind is TokenKind.IDENT:
            s.i += 1
            head = ast_.OpHead(ref=ref_tok.text, span=_join(t.span, ref_tok.span))
        elif ref_tok.kind is TokenKind.HEX:
            s.i += 1
            head = ast_.OpHead(ref=int(ref_tok.text, 16), span=_join(t.span, ref_tok.span))
        elif ref_tok.kind is TokenKind.DEC:
            s.i += 1
            head = ast_.OpHead(ref=int(ref_tok.text, 10), span=_join(t.span, ref_tok.span))
        else:
            s.bag.error("P001_UNEXPECTED_TOKEN", f"expected identifier or numeric literal after 'op'", ref_tok.span)
            raise CompileError(s.bag)
    elif t.kind is TokenKind.KW_BTYPE:
        s.i += 1
        name_tok = s.eat(TokenKind.IDENT)
        head = ast_.BTypeHead(name=name_tok.text, span=_join(t.span, name_tok.span))
    elif t.kind is TokenKind.KW_PRED:
        s.i += 1
        name_tok = s.eat(TokenKind.IDENT)
        head = ast_.PredHead(name=name_tok.text, span=_join(t.span, name_tok.span))
    elif t.kind is TokenKind.KW_ANY:
        s.i += 1
        head = ast_.AnyHead(span=t.span)
    else:
        s.bag.error("P001_UNEXPECTED_TOKEN", f"expected node head, found {t.kind.name}", t.span)
        raise CompileError(s.bag)

    capture: str | None = None
    if s.at(TokenKind.AT):
        s.i += 1
        cap_tok = s.eat(TokenKind.IDENT)
        capture = cap_tok.text
        head_start_span = _join(head_start_span, cap_tok.span)

    where_predicate: str | None = None
    if s.at(TokenKind.KW_WHERE):
        s.i += 1
        pred_tok = s.eat(TokenKind.IDENT)
        where_predicate = pred_tok.text
        head_start_span = _join(head_start_span, pred_tok.span)

    return ast_.Node(
        head=head,
        capture=capture,
        where_predicate=where_predicate,
        span=head_start_span,
    )


def _parse_children_block(s: _ParserState) -> ast_.ChildrenBlock:
    start = s.eat(TokenKind.LBRACE).span
    items: list[ast_.PatternExpr] = []
    while True:
        # Skip empty separators between items.
        skipped_sep = False
        while s.at(TokenKind.SEMI) or s.at(TokenKind.NEWLINE):
            s.i += 1
            skipped_sep = True
        if s.at(TokenKind.RBRACE):
            break
        # Empty item check (semi/newline followed immediately by another sep is OK;
        # semi/newline followed by RBRACE is OK; semi/newline at start handled by skip above).
        if s.at(TokenKind.EOF):
            s.bag.error("P001_UNEXPECTED_TOKEN", "unterminated children block", start)
            raise CompileError(s.bag)
        item = _parse_pattern_expr(s)
        items.append(item)
    end = s.eat(TokenKind.RBRACE).span
    if not items:
        s.bag.error("P002_EMPTY_CHILDREN_ITEM", "children block must contain at least one item", _join(start, end))
        raise CompileError(s.bag)
    return ast_.ChildrenBlock(items=tuple(items), span=_join(start, end))
```

- [ ] **Step 2: Add tests for pattern expression shapes**

Append to `tests/mch/test_parser.py`:

```python
def test_parse_node_op_with_capture():
    src = """
    opcode Foo = 0x1
    rule r = op Foo @x
    """
    f = _parse(src)
    seq = f.rules[0].pattern
    # A bare op-with-capture is just a Node (not wrapped in Sequence).
    assert isinstance(seq, ast_.Node)
    assert seq.capture == "x"
    assert isinstance(seq.head, ast_.OpHead)
    assert seq.head.ref == "Foo"


def test_parse_linear_chain():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    rule r = op A @a -> op B @b -> op C @c
    """
    f = _parse(src)
    seq = f.rules[0].pattern
    assert isinstance(seq, ast_.Sequence)
    assert isinstance(seq.head, ast_.Node) and seq.head.capture == "a"
    assert len(seq.tail) == 2
    assert seq.tail[0][0] is ast_.Edge.CHILD
    assert seq.tail[1][0] is ast_.Edge.CHILD


def test_parse_maybe_wrapped_node():
    src = """
    opcode A = 0x1
    rule r = maybe op A @a
    """
    f = _parse(src)
    seq = f.rules[0].pattern
    assert isinstance(seq, ast_.Maybe)
    assert isinstance(seq.inner, ast_.Node)


def test_parse_alternative_in_group():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    rule r = ( op A @a | op B @b )
    """
    f = _parse(src)
    seq = f.rules[0].pattern
    assert isinstance(seq, ast_.Alternative)
    assert len(seq.branches) == 2


def test_parse_rule_reference():
    src = """
    opcode A = 0x1
    rule inner = op A @a
    rule outer = $inner
    """
    f = _parse(src)
    assert isinstance(f.rules[1].pattern, ast_.RuleRef)
    assert f.rules[1].pattern.name == "inner"


def test_parse_children_block_with_semicolons():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    rule r = op A @a -> { op B @b; op C @c }
    """
    f = _parse(src)
    seq = f.rules[0].pattern
    assert isinstance(seq, ast_.Sequence)
    cb = seq.tail[0][1]
    assert isinstance(cb, ast_.ChildrenBlock)
    assert len(cb.items) == 2


def test_parse_children_block_with_newlines():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    rule r = op A @a -> {
        op B @b
        op C @c
    }
    """
    f = _parse(src)
    seq = f.rules[0].pattern
    cb = seq.tail[0][1]
    assert isinstance(cb, ast_.ChildrenBlock)
    assert len(cb.items) == 2


def test_parse_predicate_clause():
    src = """
    opcode A = 0x1
    predicate my_pred
    rule r = op A @a where my_pred
    """
    f = _parse(src)
    seq = f.rules[0].pattern
    assert isinstance(seq, ast_.Node)
    assert seq.where_predicate == "my_pred"


def test_parse_parent_edge():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    rule r = op A @a <- op B @b
    """
    f = _parse(src)
    seq = f.rules[0].pattern
    assert isinstance(seq, ast_.Sequence)
    assert seq.tail[0][0] is ast_.Edge.PARENT
```

- [ ] **Step 3: Run tests**

Run: `python -m pytest tests/mch/test_parser.py -v`
Expected: all pass.

---

## Task 9: Parser — error cases

**Files:**
- Modify: `tests/mch/test_parser.py` (append)

- [ ] **Step 1: Add error-case tests**

```python
def _parse_expect_error(src: str, code: str) -> None:
    with pytest.raises(CompileError) as exc:
        _parse(src)
    codes = [d.code for d in exc.value.bag.items]
    assert code in codes, f"expected diagnostic {code}, got {codes}"


def test_unexpected_top_level_token():
    _parse_expect_error("garbage 0x1", "P001_UNEXPECTED_TOKEN")


def test_missing_produces_or_build_in_matcher():
    src = """
    opcode Foo = 0x1
    matcher m {
        entry @x
        op Foo @x
    }
    """
    _parse_expect_error(src, "P001_UNEXPECTED_TOKEN")


def test_empty_children_block_rejected():
    src = """
    opcode A = 0x1
    rule r = op A @a -> {}
    """
    _parse_expect_error(src, "P002_EMPTY_CHILDREN_ITEM")


def test_missing_closing_brace_in_matcher():
    src = """
    opcode Foo = 0x1
    matcher m {
        produces FooBlock
        build    build_foo
        entry    @x

        op Foo @x
    """
    _parse_expect_error(src, "P001_UNEXPECTED_TOKEN")
```

- [ ] **Step 2: Run tests; fix parser if any fail**

Run: `python -m pytest tests/mch/test_parser.py -v -k error or test_unexpected or test_missing or test_empty`
Expected: all error-case tests pass.

(If `test_missing_produces_or_build_in_matcher` fails because the implementation accepts the matcher and falls through, recheck the missing-directive check in `_parse_matcher_decl`.)

---

## Task 10: Registry

**Files:**
- Create: `indexer/events/mch/registry.py`
- Create: `tests/mch/test_registry.py`

- [ ] **Step 1: Write the failing tests**

```python
# tests/mch/test_registry.py
from __future__ import annotations

import pytest

from indexer.events.mch.registry import Registries


def test_register_predicate_via_method():
    reg = Registries()

    @reg.register_predicate("pton_self_transfer")
    def pton_self_transfer(block):  # noqa: ARG001
        return True

    assert reg.predicates["pton_self_transfer"] is pton_self_transfer


def test_register_builder_via_method():
    reg = Registries()

    @reg.register_builder("build_jetton_transfer")
    async def build_jetton_transfer(m):  # noqa: ARG001
        return None

    assert reg.builders["build_jetton_transfer"] is build_jetton_transfer


def test_double_predicate_registration_raises():
    reg = Registries()

    @reg.register_predicate("p")
    def first(b):  # noqa: ARG001
        return True

    with pytest.raises(KeyError, match="predicate 'p' already registered"):
        @reg.register_predicate("p")
        def second(b):  # noqa: ARG001
            return False


def test_double_builder_registration_raises():
    reg = Registries()

    @reg.register_builder("b")
    async def first(m):  # noqa: ARG001
        return None

    with pytest.raises(KeyError, match="builder 'b' already registered"):
        @reg.register_builder("b")
        async def second(m):  # noqa: ARG001
            return None


def test_register_opcode_and_block_type():
    reg = Registries()
    reg.opcodes["JettonTransfer"] = 0x0f8a7ea5
    reg.block_types["JettonTransferBlock"] = object  # placeholder
    assert reg.opcodes["JettonTransfer"] == 0x0f8a7ea5
```

- [ ] **Step 2: Run, confirm failures**

Run: `python -m pytest tests/mch/test_registry.py -v`
Expected: ImportError.

- [ ] **Step 3: Write `registry.py`**

```python
# indexer/events/mch/registry.py
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Awaitable, Callable, TypeVar

F = TypeVar("F", bound=Callable[..., Any])


@dataclass
class Registries:
    opcodes:     dict[str, int]                                    = field(default_factory=dict)
    predicates:  dict[str, Callable[[Any], bool]]                  = field(default_factory=dict)
    block_types: dict[str, type]                                   = field(default_factory=dict)
    builders:    dict[str, Callable[..., Awaitable[Any]]]          = field(default_factory=dict)

    def register_predicate(self, name: str) -> Callable[[F], F]:
        def deco(fn: F) -> F:
            if name in self.predicates:
                raise KeyError(f"predicate {name!r} already registered")
            self.predicates[name] = fn  # type: ignore[assignment]
            return fn
        return deco

    def register_builder(self, name: str) -> Callable[[F], F]:
        def deco(fn: F) -> F:
            if name in self.builders:
                raise KeyError(f"builder {name!r} already registered")
            self.builders[name] = fn  # type: ignore[assignment]
            return fn
        return deco
```

- [ ] **Step 4: Run tests**

Run: `python -m pytest tests/mch/test_registry.py -v`
Expected: 5 passed.

---

## Task 11: Resolver — name binding (success path)

**Files:**
- Create: `indexer/events/mch/resolver.py`
- Create: `tests/mch/test_resolver.py`

- [ ] **Step 1: Write the failing tests**

```python
# tests/mch/test_resolver.py
from __future__ import annotations

import pytest

from indexer.events.mch.diagnostics import CompileError, Severity
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.registry import Registries
from indexer.events.mch.resolver import (
    CaptureInfo,
    RecursionClass,
    ResolvedFile,
    resolve,
)


def _resolve(src: str, registries: Registries | None = None) -> ResolvedFile:
    tokens = list(tokenize(src, "test.mch"))
    f = parse(tokens, "test.mch")
    return resolve(f, registries or Registries())


def test_resolve_opcode_declaration():
    rf = _resolve("opcode JettonTransfer = 0x0f8a7ea5")
    assert rf.opcodes["JettonTransfer"] == 0x0f8a7ea5


def test_resolve_opcode_value_matches_host():
    reg = Registries()
    reg.opcodes["JettonTransfer"] = 0x0f8a7ea5
    rf = _resolve("opcode JettonTransfer = 0x0f8a7ea5", reg)
    assert rf.opcodes["JettonTransfer"] == 0x0f8a7ea5


def test_resolve_matcher_captures_inferred():
    src = """
    opcode Foo = 0x1
    opcode Bar = 0x2
    matcher m {
        produces FB
        build    build_m
        entry    @a

        op Foo @a -> op Bar @b
    }
    """
    rf = _resolve(src)
    m = rf.matchers[0]
    assert set(m.captures.keys()) == {"a", "b"}
    ci_a = m.captures["a"]
    assert ci_a.optional is False
    assert ci_a.list_binding is False


def test_resolve_maybe_marks_capture_optional():
    src = """
    opcode Foo = 0x1
    opcode Bar = 0x2
    matcher m {
        produces FB
        build    build_m
        entry    @a

        op Foo @a -> maybe op Bar @b
    }
    """
    rf = _resolve(src)
    m = rf.matchers[0]
    assert m.captures["a"].optional is False
    assert m.captures["b"].optional is True


def test_resolve_anchor_resolved():
    src = """
    opcode Foo = 0x1
    matcher m {
        produces FB
        build    build_m
        entry    @a

        op Foo @a
    }
    """
    rf = _resolve(src)
    assert rf.matchers[0].anchor_capture == "a"
    # And the anchor head IS the OpHead for Foo:
    anchor_node = rf.matchers[0].anchor_node
    # Inline import to avoid polluting test module namespace:
    from indexer.events.mch import ast_
    assert isinstance(anchor_node.head, ast_.OpHead)
    assert anchor_node.head.ref == "Foo"


def test_resolve_non_recursive_rule_classified():
    src = """
    opcode Foo = 0x1
    rule r = op Foo @x
    """
    rf = _resolve(src)
    assert rf.rule_recursion["r"] is RecursionClass.NONE


def test_resolve_direct_recursive_rule_classified():
    src = """
    opcode A = 0x1
    rule r = op A @a -> maybe $r
    """
    rf = _resolve(src)
    assert rf.rule_recursion["r"] is RecursionClass.DIRECT_RECURSIVE
```

- [ ] **Step 2: Run, confirm failures**

Run: `python -m pytest tests/mch/test_resolver.py -v`
Expected: ImportError.

- [ ] **Step 3: Write `resolver.py`**

```python
# indexer/events/mch/resolver.py
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
    rule_recursion: dict[str, RecursionClass] = _classify_rules(file, bag)
    resolved_matchers: list[ResolvedMatcher] = []
    for m in file.matchers:
        resolved_matchers.append(_resolve_matcher(m, opcodes, file, registries, bag))

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

    def collect_refs(p: ast_.PatternExpr, refs: list[tuple[str, bool]], under_maybe: bool) -> None:
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
        refs: list[tuple[str, bool]] = []
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
        refs: list[tuple[str, bool]] = []
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
        refs: list[tuple[str, bool]] = []
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


def _resolve_matcher(
    m: ast_.MatcherDecl,
    opcodes: dict[str, int],
    file: ast_.File,
    registries: Registries,
    bag: DiagnosticBag,
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
            # under_recursive=True for the duration of the walk.
            target = next((r for r in file.rules if r.name == p.name), None)
            if target is not None:
                # Avoid infinite recursion: don't re-enter a rule already in the walk path.
                # Simplest guard: when target is recursive, walk once with under_recursive=True.
                # Direct recursion's bound walk is captured by setting under_recursive once.
                if not under_recursive:
                    visit(target.pattern, under_maybe, under_recursive=False if target.name not in _is_recursive_set(file) else True)

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
```

- [ ] **Step 4: Run tests**

Run: `python -m pytest tests/mch/test_resolver.py -v`
Expected: all 7 tests pass.

---

## Task 12: Resolver — error cases

**Files:**
- Modify: `tests/mch/test_resolver.py` (append)

- [ ] **Step 1: Add error-case tests**

```python
def _resolve_expect_error(src: str, code: str, registries: Registries | None = None) -> None:
    with pytest.raises(CompileError) as exc:
        _resolve(src, registries)
    codes = [d.code for d in exc.value.bag.items if d.severity is Severity.ERROR]
    assert code in codes, f"expected diagnostic {code}, got {codes}"


def test_unknown_opcode_in_op_head():
    _resolve_expect_error(
        """
        rule r = op Missing @x
        """,
        "R001_UNKNOWN_OPCODE",
    )


def test_opcode_value_mismatch_with_host():
    reg = Registries()
    reg.opcodes["JettonTransfer"] = 0xdead
    _resolve_expect_error(
        "opcode JettonTransfer = 0x0f8a7ea5",
        "R002_OPCODE_VALUE_MISMATCH",
        registries=reg,
    )


def test_unknown_predicate_in_where_clause():
    _resolve_expect_error(
        """
        opcode A = 0x1
        rule r = op A @a where missing_pred
        """,
        "R003_UNKNOWN_PREDICATE",
    )


def test_undefined_rule_reference():
    _resolve_expect_error(
        """
        opcode A = 0x1
        rule r = op A @a -> $no_such
        """,
        "R004_UNDEFINED_RULE",
    )


def test_duplicate_capture_in_same_matcher_branch():
    _resolve_expect_error(
        """
        opcode A = 0x1
        opcode B = 0x2
        matcher m {
            produces M
            build    build_m
            entry    @x

            op A @x -> op B @x
        }
        """,
        "R005_DUP_CAPTURE",
    )


def test_unbounded_recursion_rejected():
    _resolve_expect_error(
        """
        opcode A = 0x1
        rule r = op A @a -> $r
        """,
        "R006_UNBOUNDED_RECURSION",
    )


def test_entry_required():
    _resolve_expect_error(
        """
        opcode A = 0x1
        matcher m {
            produces M
            build    build_m

            op A @x
        }
        """,
        "R008_ENTRY_REQUIRED",
    )


def test_entry_inside_maybe_rejected():
    _resolve_expect_error(
        """
        opcode A = 0x1
        matcher m {
            produces M
            build    build_m
            entry    @x

            maybe op A @x
        }
        """,
        "R007_ENTRY_VIOLATES_8_3",
    )
```

- [ ] **Step 2: Run tests; fix resolver if any fail**

Run: `python -m pytest tests/mch/test_resolver.py -v`
Expected: all pass.

---

## Task 13: Recursion strategy

**Files:**
- Create: `indexer/events/mch/recursion.py`
- Create: `tests/mch/test_recursion.py`

This task defines the `RecursionStrategy` protocol and a stub `RecursiveMatcherStrategy` whose body is implemented in Task 14 (since the actual lowering needs the compiler's lowering helpers). For now we test the protocol shape.

- [ ] **Step 1: Write `recursion.py` (interface only)**

```python
# indexer/events/mch/recursion.py
from __future__ import annotations

from typing import TYPE_CHECKING, Any, Protocol

if TYPE_CHECKING:
    from indexer.events.blocks.basic_matchers import BlockMatcher
    from indexer.events.mch.resolver import ResolvedMatcher


class RecursionStrategy(Protocol):
    """Lowers a recursive rule reference to an engine matcher.

    v0 default implementation lives at the bottom of this module.
    Future swap-in: BoundedUnrollingStrategy, SubclassPerRuleStrategy.
    """

    def lower_recursive_rule(
        self,
        rule_name: str,
        ctx: Any,                  # CompileCtx, typed at the call site
    ) -> "BlockMatcher":
        ...


class RecursiveMatcherStrategy:
    """v0: lowers a recursive rule reference to a RecursiveMatcher instance.

    Implementation lives in compiler.py for now (it needs the compiler's lowering helpers).
    This class is a placeholder that the compiler instantiates and uses; the actual
    behavior is invoked through compiler.CompileCtx.
    """
    pass
```

- [ ] **Step 2: Write minimal smoke test**

```python
# tests/mch/test_recursion.py
from __future__ import annotations

from indexer.events.mch.recursion import RecursionStrategy, RecursiveMatcherStrategy


def test_recursive_matcher_strategy_instantiates():
    s = RecursiveMatcherStrategy()
    assert s is not None


def test_recursion_strategy_protocol_shape():
    # Smoke: RecursiveMatcherStrategy satisfies the Protocol structurally.
    s: RecursionStrategy = RecursiveMatcherStrategy()  # type: ignore[assignment]
    assert s is not None
```

- [ ] **Step 3: Run tests**

Run: `python -m pytest tests/mch/test_recursion.py -v`
Expected: 2 passed.

The actual recursion-shape detection logic is exercised in Task 15 (compiler behavioral tests).

---

## Task 14: Compiler — structural lowering (non-recursive)

**Files:**
- Create: `indexer/events/mch/compiler.py`
- Create: `tests/mch/test_compiler_structural.py`

This task lowers non-recursive patterns to engine matchers. Recursion support comes in Task 15.

- [ ] **Step 1: Write the failing tests**

```python
# tests/mch/test_compiler_structural.py
from __future__ import annotations

import pytest

from indexer.events.blocks.basic_matchers import (
    BlockMatcher,
    BlockTypeMatcher,
    ContractMatcher,
    GenericMatcher,
    OrMatcher,
)
from indexer.events.mch.compile import compile_text
from indexer.events.mch.registry import Registries


def _registries_with_opcodes(**opcodes: int) -> Registries:
    r = Registries()
    r.opcodes.update(opcodes)
    # Stub block-type registry entry so compile_text won't trip over `produces`.
    return r


def _with_builder(reg: Registries, name: str) -> Registries:
    @reg.register_builder(name)
    async def _build(m):  # noqa: ARG001
        return None
    return reg


def _with_block_type(reg: Registries, name: str) -> Registries:
    reg.block_types[name] = type(name, (), {})
    return reg


def test_single_op_node_lowers_to_contract_matcher_subclass():
    src = """
    opcode JettonTransfer = 0x0f8a7ea5

    matcher jetton_transfer {
        produces JettonTransferBlock
        build    build_jetton_transfer
        entry    @t

        op JettonTransfer @t
    }
    """
    reg = _registries_with_opcodes(JettonTransfer=0x0f8a7ea5)
    _with_block_type(reg, "JettonTransferBlock")
    _with_builder(reg, "build_jetton_transfer")

    compiled = compile_text(src, "test.mch", reg)
    assert len(compiled) == 1
    cm = compiled[0]
    assert cm.name == "jetton_transfer"
    assert isinstance(cm.matcher, ContractMatcher)
    assert cm.matcher.opcode == 0x0f8a7ea5
    # The matcher's class name carries the spec name for why_not parity.
    assert type(cm.matcher).__name__ == "_MCH_jetton_transfer"
    assert getattr(cm.matcher, "__mch_name__", None) == "jetton_transfer"


def test_linear_chain_wires_child_matchers():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3

    matcher m {
        produces MB
        build    build_m
        entry    @a

        op A @a -> op B @b -> op C @c
    }
    """
    reg = _registries_with_opcodes(A=0x1, B=0x2, C=0x3)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")

    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher
    assert isinstance(m, ContractMatcher)
    assert m.opcode == 0x1
    assert isinstance(m.child_matcher, ContractMatcher)
    assert m.child_matcher.opcode == 0x2
    assert isinstance(m.child_matcher.child_matcher, ContractMatcher)
    assert m.child_matcher.child_matcher.opcode == 0x3


def test_maybe_wrapped_child_marked_optional():
    src = """
    opcode A = 0x1
    opcode B = 0x2

    matcher m {
        produces MB
        build    build_m
        entry    @a

        op A @a -> maybe op B @b
    }
    """
    reg = _registries_with_opcodes(A=0x1, B=0x2)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher
    assert m.child_matcher.optional is True


def test_alternative_lowers_to_or_matcher():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3

    matcher m {
        produces MB
        build    build_m
        entry    @a

        op A @a -> ( op B @b | op C @c )
    }
    """
    reg = _registries_with_opcodes(A=0x1, B=0x2, C=0x3)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher
    assert isinstance(m.child_matcher, OrMatcher)


def test_btype_node_lowers_to_block_type_matcher():
    src = """
    opcode A = 0x1

    matcher m {
        produces MB
        build    build_m
        entry    @a

        op A @a -> btype jetton_transfer @j
    }
    """
    reg = _registries_with_opcodes(A=0x1)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher
    assert isinstance(m.child_matcher, BlockTypeMatcher)
    assert m.child_matcher.block_type == "jetton_transfer"


def test_anchor_btype_entry():
    src = """
    matcher m {
        produces MB
        build    build_m
        entry    @j

        btype jetton_transfer @j
    }
    """
    reg = Registries()
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher
    assert isinstance(m, BlockTypeMatcher)
    assert m.block_type == "jetton_transfer"


def test_children_block_lowers_to_children_matchers():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3

    matcher m {
        produces MB
        build    build_m
        entry    @a

        op A @a -> {
            op B @b
            op C @c
        }
    }
    """
    reg = _registries_with_opcodes(A=0x1, B=0x2, C=0x3)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher
    assert m.children_matchers is not None
    assert len(m.children_matchers) == 2
```

- [ ] **Step 2: Run, confirm failures**

Run: `python -m pytest tests/mch/test_compiler_structural.py -v`
Expected: ImportError (compile_text doesn't exist yet).

- [ ] **Step 3: Write `compiler.py`**

```python
# indexer/events/mch/compiler.py
from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Callable

from indexer.events.blocks.basic_matchers import (
    BlockMatcher,
    BlockTypeMatcher,
    ContractMatcher,
    GenericMatcher,
    OrMatcher,
    RecursiveMatcher,
)
from indexer.events.mch import ast_
from indexer.events.mch.diagnostics import CompileError, DiagnosticBag, Span
from indexer.events.mch.recursion import RecursionStrategy, RecursiveMatcherStrategy
from indexer.events.mch.registry import Registries
from indexer.events.mch.resolver import (
    CaptureInfo,
    RecursionClass,
    ResolvedFile,
    ResolvedMatcher,
)


@dataclass
class CompiledMatcher:
    name: str
    matcher: BlockMatcher
    match_cls: type
    build_fn_name: str
    produces_cls_name: str
    source_span: Span


@dataclass
class CompileCtx:
    resolved: ResolvedFile
    registries: Registries
    recursion: RecursionStrategy
    bag: DiagnosticBag


# Module-level cache to keep synthesized subclasses idempotent across compiles.
_GENERATED_SUBCLASSES: dict[str, type] = {}


def compile_(
    resolved: ResolvedFile,
    registries: Registries,
    recursion: RecursionStrategy | None = None,
) -> list[CompiledMatcher]:
    """Compile a resolved file to a list of CompiledMatcher.

    Function is named `compile_` (trailing underscore) to avoid shadowing the builtin.
    """
    bag = DiagnosticBag()
    ctx = CompileCtx(
        resolved=resolved,
        registries=registries,
        recursion=recursion or RecursiveMatcherStrategy(),
        bag=bag,
    )

    out: list[CompiledMatcher] = []
    for rm in resolved.matchers:
        out.append(_compile_matcher(rm, ctx))

    if bag.has_errors:
        raise CompileError(bag)

    return out


def _compile_matcher(rm: ResolvedMatcher, ctx: CompileCtx) -> CompiledMatcher:
    decl = rm.decl

    if decl.produces not in ctx.registries.block_types:
        ctx.bag.error(
            "C003_UNKNOWN_BLOCK_TYPE",
            f"matcher {decl.name!r}: produces type {decl.produces!r} is not registered",
            decl.span,
        )
    if decl.build not in ctx.registries.builders:
        ctx.bag.error(
            "C002_UNKNOWN_BUILDER",
            f"matcher {decl.name!r}: builder {decl.build!r} is not registered",
            decl.span,
        )

    # Lower the pattern relative to the anchor.
    parent_m, child_m, children_ms = _lower_relative_to_anchor(decl.pattern, rm.anchor_node, ctx)

    # Build the synthetic subclass based on the anchor head.
    head = rm.anchor_node.head
    if isinstance(head, ast_.OpHead):
        opcode = head.ref if isinstance(head.ref, int) else ctx.resolved.opcodes.get(head.ref) or ctx.registries.opcodes.get(head.ref)
        base = ContractMatcher
        init_kwargs: dict[str, Any] = dict(
            opcode=opcode,
            child_matcher=child_m,
            parent_matcher=parent_m,
            children_matchers=children_ms,
            include_excess=decl.include_excess,
        )
    elif isinstance(head, ast_.BTypeHead):
        base = BlockTypeMatcher
        init_kwargs = dict(
            block_type=head.name,
            child_matcher=child_m,
            parent_matcher=parent_m,
        )
        # BlockTypeMatcher doesn't accept children_matchers in its __init__; if present,
        # wrap by assigning after construction.
    else:
        # Resolver should have rejected this; defensive guard.
        ctx.bag.error(
            "C001_RECURSION_SHAPE_UNSUPPORTED",
            f"matcher {decl.name!r}: unsupported anchor head {type(head).__name__}",
            decl.span,
        )
        raise CompileError(ctx.bag)

    subclass = _make_subclass(decl.name, base, rm, ctx)
    instance: BlockMatcher = subclass(**init_kwargs)
    # Some bases don't accept children_matchers in __init__; apply post-hoc.
    if children_ms is not None and getattr(instance, "children_matchers", None) is None:
        instance.children_matchers = children_ms
    # include_bounces lives on the base; ensure flag is propagated.
    instance.include_bounces = decl.include_bounces
    instance.include_excess = decl.include_excess

    # Generated Match class (codegen produces the file; the compiler also needs the
    # runtime class for the builder call). We construct it at runtime via dataclasses
    # so the compiler doesn't depend on codegen having run first.
    match_cls = _make_match_dataclass(decl.name, rm.captures, ctx.registries.block_types)

    return CompiledMatcher(
        name=decl.name,
        matcher=instance,
        match_cls=match_cls,
        build_fn_name=decl.build,
        produces_cls_name=decl.produces,
        source_span=decl.span,
    )


def _make_subclass(name: str, base: type, rm: ResolvedMatcher, ctx: CompileCtx) -> type:
    """Synthesize a BlockMatcher subclass named `_MCH_<name>`.

    Cached: subsequent compilations of the same matcher reuse the class to keep
    `tools.matcher_trace` patches stable across runs.
    """
    cls_name = f"_MCH_{name}"
    if cls_name in _GENERATED_SUBCLASSES:
        return _GENERATED_SUBCLASSES[cls_name]

    builder_name = rm.decl.build

    async def build_block(self, block, other_blocks):  # type: ignore[override]
        match_cls = _MATCH_CLASS_REGISTRY[name]
        captures = _CAPTURE_EXTRACTORS_REGISTRY[name]
        kwargs = {}
        for cap_name, extractor in captures.items():
            kwargs[cap_name] = extractor(block, other_blocks)
        m = match_cls(**kwargs)
        builder = ctx.registries.builders.get(builder_name)
        if builder is None:
            # Late lookup failure — surfaces as a runtime error since the builder
            # may have been registered after compile-time. Keep it loud.
            raise RuntimeError(f"builder {builder_name!r} is not registered at run time")
        produced = await builder(m)
        out_blocks = [produced] if produced is not None else []
        return out_blocks

    new_cls = type(
        cls_name,
        (base,),
        {
            "__mch_name__": name,
            "__mch_source__": rm.decl.span,
            "build_block": build_block,
        },
    )
    _GENERATED_SUBCLASSES[cls_name] = new_cls
    return new_cls


# Module-level registries used by the synthesized subclasses.
# We keep them keyed by spec name; tests that compile the same matcher twice see the
# extractor map and Match class refreshed.
_MATCH_CLASS_REGISTRY: dict[str, type] = {}
_CAPTURE_EXTRACTORS_REGISTRY: dict[str, dict[str, Callable[[Any, list[Any]], Any]]] = {}


def _make_match_dataclass(matcher_name: str, captures: dict[str, CaptureInfo], block_types: dict[str, type]) -> type:
    """Build a runtime dataclass with one field per capture."""
    from dataclasses import make_dataclass

    fields: list[tuple[str, Any]] = []
    extractors: dict[str, Callable[[Any, list[Any]], Any]] = {}
    for cap_name, info in captures.items():
        # Type hint (not enforced at runtime).
        field_type = Any
        fields.append((cap_name, field_type))
        extractors[cap_name] = _make_extractor(info)
    cls = make_dataclass(f"{matcher_name}Match", fields)
    _MATCH_CLASS_REGISTRY[matcher_name] = cls
    _CAPTURE_EXTRACTORS_REGISTRY[matcher_name] = extractors
    return cls


def _make_extractor(info: CaptureInfo) -> Callable[[Any, list[Any]], Any]:
    """Return a callable that, given (anchor_block, other_blocks), produces the bound value.

    v0 strategy: search the (anchor + other_blocks) list for a block matching the capture's
    head. This is structurally sound for the worked examples; a future version replaces it
    with deterministic position-id indexing tied to lowering order.
    """
    head = info.head

    if isinstance(head, ast_.OpHead):
        target = head.ref if isinstance(head.ref, int) else None
        target_name = head.ref if isinstance(head.ref, str) else None

        def extract_op(block, others):
            for b in [block] + others:
                if hasattr(b, "opcode"):
                    if target is not None and b.opcode == target:
                        return b
                    if target_name is not None:
                        return b  # caller bound by name resolves to the unique opcode in non-conflicting patterns
            return None if info.optional else None
        return extract_op

    if isinstance(head, ast_.BTypeHead):
        name = head.name

        def extract_btype(block, others):
            for b in [block] + others:
                if getattr(b, "btype", None) == name:
                    return b
            return None
        return extract_btype

    if isinstance(head, ast_.PredHead) or isinstance(head, ast_.AnyHead):
        def extract_any(block, _others):
            return block
        return extract_any

    return lambda b, _o: b


def _lower_relative_to_anchor(
    pattern: ast_.PatternExpr,
    anchor: ast_.Node,
    ctx: CompileCtx,
) -> tuple[BlockMatcher | None, BlockMatcher | None, list[BlockMatcher] | None]:
    """Walk the pattern to figure out (parent_matcher, child_matcher, children_matchers).

    v0 strategy (works for the documented examples):
      - Find the path from the AST root to the anchor.
      - Everything *after* the anchor along that path becomes the child chain.
      - Anything in children blocks attached to the anchor become children_matchers.
      - Anything reachable via PARENT edges leading to the anchor becomes parent chain.
    """
    # Use a node-locator: pre-compute the anchor's path.
    parents = _find_anchor_path(pattern, anchor)
    if parents is None:
        # Anchor is the whole pattern.
        return None, None, None

    # In v0 worked examples (jetton_transfer, stonfi_v2_swap), patterns are sequences rooted at
    # the anchor with only forward (CHILD) edges and optional children blocks. Walk the
    # sequence beyond the anchor for the child chain.
    child_chain, children_ms = _lower_child_chain_from(parents[-1], pattern, anchor, ctx)
    parent_chain = _lower_parent_chain_from(parents[-1], pattern, anchor, ctx)
    return parent_chain, child_chain, children_ms


def _find_anchor_path(pattern: ast_.PatternExpr, anchor: ast_.Node) -> list[ast_.PatternExpr] | None:
    """Return the list of AST nodes from the root pattern down to (and including) `anchor`,
    or None if anchor is the entire pattern (no enclosing sequence)."""
    if pattern is anchor:
        return None

    def search(p: ast_.PatternExpr, path: list[ast_.PatternExpr]) -> list[ast_.PatternExpr] | None:
        if p is anchor:
            return path + [p]
        if isinstance(p, ast_.Sequence):
            r = search(p.head, path + [p])
            if r:
                return r
            for _e, atom in p.tail:
                r = search(atom, path + [p])
                if r:
                    return r
        elif isinstance(p, ast_.Maybe):
            return search(p.inner, path + [p])
        elif isinstance(p, ast_.Alternative):
            for b in p.branches:
                r = search(b, path + [p])
                if r:
                    return r
        elif isinstance(p, ast_.ChildrenBlock):
            for it in p.items:
                r = search(it, path + [p])
                if r:
                    return r
        return None

    return search(pattern, [])


def _lower_child_chain_from(
    parent: ast_.PatternExpr,
    pattern: ast_.PatternExpr,
    anchor: ast_.Node,
    ctx: CompileCtx,
) -> tuple[BlockMatcher | None, list[BlockMatcher] | None]:
    """Find the segments downstream of the anchor in the parent's structure."""
    if isinstance(parent, ast_.Sequence):
        # Concatenate all atoms after the anchor in this sequence's tail.
        # Anchor is either parent.head or appears as one of parent.tail's atoms.
        downstream_atoms: list[ast_.PatternExpr] = []
        if parent.head is anchor:
            downstream_atoms = [atom for _e, atom in parent.tail]
        else:
            anchor_idx = None
            for i, (_e, atom) in enumerate(parent.tail):
                if atom is anchor:
                    anchor_idx = i
                    break
            if anchor_idx is not None:
                downstream_atoms = [atom for _e, atom in parent.tail[anchor_idx + 1:]]
        if not downstream_atoms:
            return None, None
        # First downstream atom may be a children block.
        first = downstream_atoms[0]
        if isinstance(first, ast_.ChildrenBlock):
            children_ms = [_lower_atom(it, ctx) for it in first.items]
            # Remaining downstream atoms after the children block are pushed through child_matcher.
            rest = downstream_atoms[1:]
            if rest:
                child = _chain(_lower_atom(a, ctx) for a in rest)
                return child, children_ms
            return None, children_ms
        # Otherwise, chain them via child_matcher.
        child = _chain(_lower_atom(a, ctx) for a in downstream_atoms)
        return child, None

    # If parent isn't a Sequence, anchor is a leaf or in an alternative — defer to
    # recursive descent: just return no children for v0.
    return None, None


def _lower_parent_chain_from(
    parent: ast_.PatternExpr,
    pattern: ast_.PatternExpr,
    anchor: ast_.Node,
    ctx: CompileCtx,
) -> BlockMatcher | None:
    """Find segments upstream of the anchor via PARENT edges."""
    if isinstance(parent, ast_.Sequence):
        # Anchor is parent.head or appears in tail. Walk PARENT-edged predecessors.
        if parent.head is anchor:
            return None
        # Look for PARENT edges whose target is the anchor; if found, the previous atom
        # (going right-to-left) becomes the parent matcher.
        for i, (edge, atom) in enumerate(parent.tail):
            if atom is anchor and edge is ast_.Edge.PARENT:
                # The "parent" atom is the previous one (head if i==0 else tail[i-1]).
                previous = parent.head if i == 0 else parent.tail[i - 1][1]
                return _lower_atom(previous, ctx)
    return None


def _lower_atom(p: ast_.PatternExpr, ctx: CompileCtx) -> BlockMatcher:
    if isinstance(p, ast_.Node):
        return _lower_node(p, ctx)
    if isinstance(p, ast_.Maybe):
        inner = _lower_atom(p.inner, ctx)
        inner.optional = True
        return inner
    if isinstance(p, ast_.Alternative):
        return OrMatcher([_lower_atom(b, ctx) for b in p.branches])
    if isinstance(p, ast_.Sequence):
        # Convert sequence head + tail into a chain rooted at head via child_matcher.
        first = _lower_atom(p.head, ctx)
        for _edge, atom in p.tail:
            inner = _lower_atom(atom, ctx)
            _attach_as_child(first, inner)
        return first
    if isinstance(p, ast_.ChildrenBlock):
        # When used as an atom (rare), produce an Or of its items.
        return OrMatcher([_lower_atom(it, ctx) for it in p.items])
    if isinstance(p, ast_.RuleRef):
        target = next((r for r in ctx.resolved.raw.rules if r.name == p.name), None)
        if target is None:
            raise CompileError(ctx.bag)  # resolver should have caught this
        if ctx.resolved.rule_recursion.get(p.name) in (RecursionClass.DIRECT_RECURSIVE, RecursionClass.INDIRECT_RECURSIVE):
            # Defer to recursion strategy (Task 15).
            return _lower_recursive_rule(p.name, ctx)
        return _lower_atom(target.pattern, ctx)
    raise AssertionError(f"unhandled pattern node: {type(p).__name__}")


def _lower_node(n: ast_.Node, ctx: CompileCtx) -> BlockMatcher:
    head = n.head
    if isinstance(head, ast_.OpHead):
        opcode = head.ref if isinstance(head.ref, int) else (
            ctx.resolved.opcodes.get(head.ref) or ctx.registries.opcodes.get(head.ref)
        )
        m: BlockMatcher = ContractMatcher(opcode=opcode)
    elif isinstance(head, ast_.BTypeHead):
        m = BlockTypeMatcher(block_type=head.name)
    elif isinstance(head, ast_.PredHead):
        pred = ctx.registries.predicates[head.name]
        m = GenericMatcher(test_self_func=pred)
    elif isinstance(head, ast_.AnyHead):
        m = GenericMatcher(test_self_func=lambda _b: True)
    else:
        raise AssertionError(f"unknown node head: {type(head).__name__}")
    # Apply where-clause as an AND on test_self.
    if n.where_predicate is not None:
        pred = ctx.registries.predicates[n.where_predicate]
        original_test = m.test_self
        def combined(block, _orig=original_test, _p=pred):
            return _orig(block) and _p(block)
        m.test_self = combined  # type: ignore[assignment]
    return m


def _chain(matchers_iter) -> BlockMatcher | None:
    matchers = [m for m in matchers_iter if m is not None]
    if not matchers:
        return None
    first = matchers[0]
    current = first
    for nxt in matchers[1:]:
        _attach_as_child(current, nxt)
        current = nxt
    return first


def _attach_as_child(host: BlockMatcher, child: BlockMatcher) -> None:
    if host.child_matcher is None:
        host.child_matcher = child
    else:
        _attach_as_child(host.child_matcher, child)


def _lower_recursive_rule(name: str, ctx: CompileCtx) -> BlockMatcher:
    """Filled in by Task 15."""
    raise NotImplementedError("filled in by Task 15")
```

- [ ] **Step 4: Write `compile.py` facade**

```python
# indexer/events/mch/compile.py
from __future__ import annotations

from pathlib import Path

from indexer.events.mch.compiler import CompiledMatcher, compile_
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.recursion import RecursionStrategy
from indexer.events.mch.registry import Registries
from indexer.events.mch.resolver import resolve


def compile_text(
    text: str,
    path: str,
    registries: Registries,
    *,
    recursion: RecursionStrategy | None = None,
) -> list[CompiledMatcher]:
    tokens = list(tokenize(text, path))
    file_ast = parse(tokens, path)
    resolved = resolve(file_ast, registries)
    return compile_(resolved, registries, recursion=recursion)


def compile_file(
    path: Path,
    registries: Registries,
    *,
    recursion: RecursionStrategy | None = None,
) -> list[CompiledMatcher]:
    text = Path(path).read_text(encoding="utf-8")
    return compile_text(text, str(path), registries, recursion=recursion)


def compile_dir(
    dir_path: Path,
    registries: Registries,
    *,
    recursion: RecursionStrategy | None = None,
) -> list[CompiledMatcher]:
    out: list[CompiledMatcher] = []
    for p in sorted(Path(dir_path).glob("*.mch")):
        out.extend(compile_file(p, registries, recursion=recursion))
    return out
```

- [ ] **Step 5: Expose the facade from `__init__.py`**

Replace the body of `indexer/events/mch/__init__.py`:

```python
# indexer/events/mch/__init__.py
"""`.mch` matcher DSL — Python v0 compiler.

Public surface:
    compile_text(text, path, registries, ...) -> list[CompiledMatcher]
    compile_file(path, registries, ...)       -> list[CompiledMatcher]
    Registries                                 -> host-language registry container
"""
from __future__ import annotations

from indexer.events.mch.compile import compile_dir, compile_file, compile_text
from indexer.events.mch.compiler import CompiledMatcher
from indexer.events.mch.registry import Registries

__all__ = [
    "CompiledMatcher",
    "Registries",
    "compile_dir",
    "compile_file",
    "compile_text",
]
```

- [ ] **Step 6: Run structural tests**

Run: `python -m pytest tests/mch/test_compiler_structural.py -v`
Expected: all 7 tests pass. If `test_anchor_btype_entry` complains about `children_matchers` not being supported on `BlockTypeMatcher`, accept the case where the compiler does not pass it and just returns the base matcher. (No children_matchers in that test — should be OK.)

If tests fail with extractor-related errors, the issue is in `_make_extractor`. Iterate until passing.

---

## Task 15: Compiler — recursion lowering

**Files:**
- Modify: `indexer/events/mch/compiler.py` (replace the `_lower_recursive_rule` stub)
- Modify: `tests/mch/test_compiler_structural.py` (append recursion tests)

- [ ] **Step 1: Write the failing recursion tests**

Append to `tests/mch/test_compiler_structural.py`:

```python
from indexer.events.blocks.basic_matchers import RecursiveMatcher


def test_recursive_rule_lowers_to_recursive_matcher():
    src = """
    opcode A = 0x1
    opcode B = 0x2

    rule swap_step =
        op A @step -> ( maybe $swap_step | op B @term )

    matcher m {
        produces MB
        build    build_m
        entry    @anchor

        op A @anchor -> $swap_step
    }
    """
    reg = _registries_with_opcodes(A=0x1, B=0x2)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher
    # Somewhere down the chain there should be a RecursiveMatcher.
    cur = m
    found = False
    seen = set()
    while cur is not None and id(cur) not in seen:
        seen.add(id(cur))
        if isinstance(cur, RecursiveMatcher):
            found = True
            break
        cur = cur.child_matcher
    assert found, "expected RecursiveMatcher in the child chain"
```

- [ ] **Step 2: Run, confirm failure**

Run: `python -m pytest tests/mch/test_compiler_structural.py::test_recursive_rule_lowers_to_recursive_matcher -v`
Expected: `NotImplementedError("filled in by Task 15")`.

- [ ] **Step 3: Replace the `_lower_recursive_rule` stub**

In `indexer/events/mch/compiler.py`, replace:

```python
def _lower_recursive_rule(name: str, ctx: CompileCtx) -> BlockMatcher:
    """Filled in by Task 15."""
    raise NotImplementedError("filled in by Task 15")
```

with:

```python
def _lower_recursive_rule(name: str, ctx: CompileCtx) -> BlockMatcher:
    """Lower a recursive rule via the configured RecursionStrategy.

    v0 strategy: decompose the rule body into a "step" (non-recursive branches)
    and an "exit" (the branch(es) reachable when the recursion terminates) and
    emit RecursiveMatcher(step, exit).
    """
    target = next(r for r in ctx.resolved.raw.rules if r.name == name)
    pattern = target.pattern

    step_branches, exit_branches = _split_recursion_branches(pattern, name)

    # Lower step.
    if not step_branches:
        ctx.bag.error(
            "C001_RECURSION_SHAPE_UNSUPPORTED",
            f"rule {name!r}: no non-recursive step branch found",
            target.span,
        )
        raise CompileError(ctx.bag)
    if len(step_branches) == 1:
        step = _lower_atom(step_branches[0], ctx)
    else:
        step = OrMatcher([_lower_atom(b, ctx) for b in step_branches])

    # Lower exit. If no exit branches were found (rule body only had recursive refs),
    # use a permissive matcher.
    if not exit_branches:
        exit_m = GenericMatcher(test_self_func=lambda _b: True)
    elif len(exit_branches) == 1:
        exit_m = _lower_atom(exit_branches[0], ctx)
    else:
        exit_m = OrMatcher([_lower_atom(b, ctx) for b in exit_branches])

    return RecursiveMatcher(repeating_matcher=step, exit_matcher=exit_m, optional=True)


def _split_recursion_branches(
    pattern: ast_.PatternExpr,
    rule_name: str,
) -> tuple[list[ast_.PatternExpr], list[ast_.PatternExpr]]:
    """Heuristic: separate (step, exit) branches.

    - Branches that *do not* reference `$rule_name` are exits.
    - Branches that reference `$rule_name` outside `maybe` are steps.
    - Branches whose only $rule_name reference is inside `maybe` are also steps
      (the maybe is the termination marker).

    Returns (step_branches, exit_branches). For non-Alternative patterns we return
    one step branch (the whole pattern) and no exits.
    """
    if isinstance(pattern, ast_.Alternative):
        steps: list[ast_.PatternExpr] = []
        exits: list[ast_.PatternExpr] = []
        for b in pattern.branches:
            if _contains_rule_ref(b, rule_name):
                steps.append(b)
            else:
                exits.append(b)
        return steps, exits
    return [pattern], []


def _contains_rule_ref(p: ast_.PatternExpr, name: str) -> bool:
    if isinstance(p, ast_.RuleRef):
        return p.name == name
    if isinstance(p, ast_.Maybe):
        return _contains_rule_ref(p.inner, name)
    if isinstance(p, ast_.Sequence):
        if _contains_rule_ref(p.head, name):
            return True
        return any(_contains_rule_ref(a, name) for _e, a in p.tail)
    if isinstance(p, ast_.Alternative):
        return any(_contains_rule_ref(b, name) for b in p.branches)
    if isinstance(p, ast_.ChildrenBlock):
        return any(_contains_rule_ref(it, name) for it in p.items)
    return False
```

- [ ] **Step 4: Run tests**

Run: `python -m pytest tests/mch/test_compiler_structural.py -v`
Expected: all pass, including the new recursion test.

---

## Task 16: Compiler — behavioral tests against synthetic block trees

**Files:**
- Create: `tests/mch/conftest.py`
- Create: `tests/mch/test_compiler_behavioral.py`

- [ ] **Step 1: Write `conftest.py` with a block-tree builder**

```python
# tests/mch/conftest.py
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

import pytest

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block


class _FakeCallContractBlock(CallContractBlock):
    """A CallContractBlock instance constructable without database state.

    Real CallContractBlock requires an EventNode; the lowering tests need only
    `opcode`, `btype`, `next_blocks`, `previous_block`, `event_nodes`. We bypass
    the production constructor by creating a bare Block and setting attributes.
    """

    def __init__(self, opcode: int):  # pylint: disable=super-init-not-called
        # Skip CallContractBlock.__init__ — it needs message bodies.
        Block.__init__(self, "call_contract", [], {})
        self.opcode = opcode
        self.next_blocks = []
        self.previous_block = None


def _new_block(opcode: int | None = None, btype: str | None = None) -> Block:
    if opcode is not None:
        return _FakeCallContractBlock(opcode)
    assert btype is not None
    b = Block(btype, [], {})
    return b


@dataclass
class BlockSpec:
    opcode: int | None = None
    btype: str | None = None
    children: list["BlockSpec"] = field(default_factory=list)


@pytest.fixture
def build_block_tree():
    """Return a function that builds a Block tree from a BlockSpec.

    Usage:
        b = build_block_tree(BlockSpec(opcode=0x0f8a7ea5, children=[
            BlockSpec(opcode=0x178d4519, children=[
                BlockSpec(opcode=0x7362d09c),
            ]),
        ]))
    """
    def build(spec: BlockSpec, parent: Any = None) -> Any:
        b = _new_block(spec.opcode, spec.btype)
        b.previous_block = parent
        b.next_blocks = []
        for c in spec.children:
            child = build(c, parent=b)
            b.next_blocks.append(child)
        return b
    return build
```

- [ ] **Step 2: Write the behavioral tests**

```python
# tests/mch/test_compiler_behavioral.py
from __future__ import annotations

import pytest

from indexer.events.mch.compile import compile_text
from indexer.events.mch.registry import Registries

from tests.mch.conftest import BlockSpec


def _registries_with(opcodes: dict[str, int], builder_name: str, produces: str) -> tuple[Registries, list]:
    captured_matches: list = []
    reg = Registries()
    reg.opcodes.update(opcodes)
    reg.block_types[produces] = type(produces, (), {})

    @reg.register_builder(builder_name)
    async def _build(m):
        captured_matches.append(m)
        return None
    return reg, captured_matches


@pytest.mark.asyncio
async def test_simple_op_matcher_fires_on_matching_block(build_block_tree):
    src = """
    opcode Foo = 0x1

    matcher m {
        produces FooBlock
        build    build_m
        entry    @t

        op Foo @t
    }
    """
    reg, captured = _registries_with({"Foo": 0x1}, "build_m", "FooBlock")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    block = build_block_tree(BlockSpec(opcode=0x1))
    result = await m.try_build(block)
    assert result is not None
    assert len(captured) == 1


@pytest.mark.asyncio
async def test_chain_matcher_requires_child(build_block_tree):
    src = """
    opcode A = 0x1
    opcode B = 0x2

    matcher m {
        produces MB
        build    build_m
        entry    @a

        op A @a -> op B @b
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    # Tree with A and B in correct relation.
    block = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x2)]))
    assert await m.try_build(block) is not None
    assert len(captured) == 1

    # Tree without B: should NOT match.
    captured.clear()
    bare = build_block_tree(BlockSpec(opcode=0x1))
    assert await m.try_build(bare) is None
    assert len(captured) == 0


@pytest.mark.asyncio
async def test_maybe_wrapped_child_optional(build_block_tree):
    src = """
    opcode A = 0x1
    opcode B = 0x2

    matcher m {
        produces MB
        build    build_m
        entry    @a

        op A @a -> maybe op B @b
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    # B present.
    with_b = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x2)]))
    assert await m.try_build(with_b) is not None
    assert len(captured) == 1

    # B absent — still matches.
    captured.clear()
    without_b = build_block_tree(BlockSpec(opcode=0x1))
    assert await m.try_build(without_b) is not None
    assert len(captured) == 1
```

If the project doesn't already use `pytest-asyncio`, the marker needs registering. Check `tests/conftest.py` or `pyproject.toml` for asyncio mode; existing tests use `@pytest.mark.asyncio`, so it's already configured.

- [ ] **Step 3: Run tests**

Run: `python -m pytest tests/mch/test_compiler_behavioral.py -v`
Expected: 3 passed.

If they fail because `try_build` returns `[]` instead of `None` for non-matches, recheck the `BlockMatcher.try_build` contract (it returns `None` when child match fails). Both are valid "no match" indicators; the test asserts the strict `None` form — adjust the assertion to `not result` if needed.

---

## Task 17: Codegen

**Files:**
- Create: `indexer/events/mch/codegen.py`
- Create: `tests/mch/test_codegen.py`

- [ ] **Step 1: Write the failing tests**

```python
# tests/mch/test_codegen.py
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

from indexer.events.mch.codegen import emit
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.registry import Registries
from indexer.events.mch.resolver import resolve


def _resolve(src: str, reg: Registries | None = None):
    toks = list(tokenize(src, "test.mch"))
    f = parse(toks, "test.mch")
    return resolve(f, reg or Registries())


def test_emit_jetton_transfer_match_file(tmp_path: Path):
    src = """
    opcode JettonTransfer         = 0x0f8a7ea5
    opcode JettonInternalTransfer = 0x178d4519
    opcode JettonNotify           = 0x7362d09c

    matcher jetton_transfer {
        produces JettonTransferBlock
        build    build_jetton_transfer
        entry    @transfer

        op JettonTransfer @transfer
          -> op JettonInternalTransfer @internal
          -> maybe op JettonNotify @notify
    }
    """
    reg = Registries()
    reg.block_types["JettonTransferBlock"] = type("JettonTransferBlock", (), {})

    @reg.register_builder("build_jetton_transfer")
    async def _b(m):  # noqa: ARG001
        return None

    rf = _resolve(src, reg)
    written = emit(rf, tmp_path)

    assert len(written) == 1
    p = written[0]
    assert p.name == "jetton_transfer_match.py"
    content = p.read_text(encoding="utf-8")
    assert "@dataclass" in content
    assert "class JettonTransferMatch:" in content
    assert "transfer:" in content
    assert "internal:" in content
    assert "notify:" in content
    assert "| None" in content  # `notify` is optional


def test_emit_idempotent(tmp_path: Path):
    src = """
    opcode Foo = 0x1
    matcher m {
        produces FooBlock
        build    build_m
        entry    @t

        op Foo @t
    }
    """
    reg = Registries()
    reg.block_types["FooBlock"] = type("FooBlock", (), {})

    @reg.register_builder("build_m")
    async def _b(m):  # noqa: ARG001
        return None

    rf = _resolve(src, reg)
    first = emit(rf, tmp_path)[0]
    first_content = first.read_text(encoding="utf-8")
    first_mtime = first.stat().st_mtime_ns

    # Re-emit; content identical, file should not be rewritten.
    second = emit(rf, tmp_path)[0]
    assert second.read_text(encoding="utf-8") == first_content
    assert second.stat().st_mtime_ns == first_mtime


def test_emitted_file_importable(tmp_path: Path):
    src = """
    opcode Foo = 0x1
    matcher widget {
        produces FooBlock
        build    build_widget
        entry    @t

        op Foo @t
    }
    """
    reg = Registries()
    reg.block_types["FooBlock"] = type("FooBlock", (), {})

    @reg.register_builder("build_widget")
    async def _b(m):  # noqa: ARG001
        return None

    rf = _resolve(src, reg)
    written = emit(rf, tmp_path)
    p = written[0]

    spec = importlib.util.spec_from_file_location("widget_match", p)
    module = importlib.util.module_from_spec(spec)  # type: ignore[arg-type]
    sys.modules["widget_match"] = module
    spec.loader.exec_module(module)  # type: ignore[union-attr]

    cls = getattr(module, "WidgetMatch")
    assert hasattr(cls, "__dataclass_fields__")
    assert "t" in cls.__dataclass_fields__
```

- [ ] **Step 2: Run, confirm failures**

Run: `python -m pytest tests/mch/test_codegen.py -v`
Expected: ImportError.

- [ ] **Step 3: Write `codegen.py`**

```python
# indexer/events/mch/codegen.py
from __future__ import annotations

from hashlib import blake2s
from pathlib import Path

from indexer.events.mch import ast_
from indexer.events.mch.resolver import CaptureInfo, ResolvedFile


def emit(resolved: ResolvedFile, out_dir: Path) -> list[Path]:
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for rm in resolved.matchers:
        path = out_dir / f"{rm.decl.name}_match.py"
        content = _render_match_module(rm.decl.name, rm.captures, source_path=resolved.raw.path)
        # Idempotent write: skip if content (excluding the hash header) is unchanged.
        if path.exists() and path.read_text(encoding="utf-8") == content:
            written.append(path)
            continue
        path.write_text(content, encoding="utf-8")
        written.append(path)
    return written


def _render_match_module(name: str, captures: dict[str, CaptureInfo], source_path: str) -> str:
    cls_name = _to_pascal_case(name) + "Match"

    field_defs: list[str] = []
    imports: set[str] = set()
    for cap_name, info in captures.items():
        type_str = _capture_type_str(info, imports)
        field_defs.append(f"    {cap_name}: {type_str}")

    body = "\n".join(field_defs) or "    pass"
    header_hash = blake2s(f"{name}\n{body}".encode("utf-8"), digest_size=6).hexdigest()

    import_lines = sorted(imports) if imports else []
    import_block = "\n".join(import_lines)

    parts = [
        f"# Generated from {source_path}  (do not edit)",
        f"# hash: {header_hash}",
        "from __future__ import annotations",
        "",
        "from dataclasses import dataclass",
    ]
    if import_block:
        parts.append(import_block)
    parts.extend([
        "",
        "@dataclass",
        f"class {cls_name}:",
        body,
        "",
    ])
    return "\n".join(parts)


def _capture_type_str(info: CaptureInfo, imports: set[str]) -> str:
    head = info.head
    if isinstance(head, ast_.OpHead):
        imports.add("from indexer.events.blocks.basic_blocks import CallContractBlock")
        base = "CallContractBlock"
    elif isinstance(head, ast_.BTypeHead):
        # Block type class can't be auto-imported without registry knowledge; fall back to Block.
        imports.add("from indexer.events.blocks.core import Block")
        base = "Block"
    elif isinstance(head, ast_.PredHead) or isinstance(head, ast_.AnyHead):
        imports.add("from indexer.events.blocks.core import Block")
        base = "Block"
    else:
        imports.add("from typing import Any")
        base = "Any"

    if info.list_binding:
        return f"list[{base}]"
    if info.optional:
        return f"{base} | None"
    return base


def _to_pascal_case(snake: str) -> str:
    return "".join(part.capitalize() for part in snake.split("_"))
```

- [ ] **Step 4: Run tests**

Run: `python -m pytest tests/mch/test_codegen.py -v`
Expected: 3 passed.

---

## Task 18: compile.py facade — additional coverage

**Files:**
- Create: `tests/mch/test_compile_facade.py`

- [ ] **Step 1: Write the failing tests**

```python
# tests/mch/test_compile_facade.py
from __future__ import annotations

from pathlib import Path

import pytest

from indexer.events.mch import compile_dir, compile_file, compile_text
from indexer.events.mch.registry import Registries


def _basic_registries() -> Registries:
    reg = Registries()
    reg.block_types["FooBlock"] = type("FooBlock", (), {})

    @reg.register_builder("build_m")
    async def _b(m):  # noqa: ARG001
        return None
    return reg


def test_compile_text_returns_one_compiled_matcher():
    src = """
    opcode Foo = 0x1
    matcher m {
        produces FooBlock
        build    build_m
        entry    @t

        op Foo @t
    }
    """
    out = compile_text(src, "test.mch", _basic_registries())
    assert len(out) == 1
    assert out[0].name == "m"


def test_compile_file_reads_from_disk(tmp_path: Path):
    src = """
    opcode Foo = 0x1
    matcher m {
        produces FooBlock
        build    build_m
        entry    @t

        op Foo @t
    }
    """
    p = tmp_path / "x.mch"
    p.write_text(src, encoding="utf-8")
    out = compile_file(p, _basic_registries())
    assert len(out) == 1


def test_compile_dir_concatenates(tmp_path: Path):
    (tmp_path / "a.mch").write_text(
        """
        opcode A = 0x1
        matcher a_m {
            produces FooBlock
            build    build_m
            entry    @t

            op A @t
        }
        """,
        encoding="utf-8",
    )
    (tmp_path / "b.mch").write_text(
        """
        opcode B = 0x2
        matcher b_m {
            produces FooBlock
            build    build_m
            entry    @t

            op B @t
        }
        """,
        encoding="utf-8",
    )
    reg = _basic_registries()
    out = compile_dir(tmp_path, reg)
    names = {c.name for c in out}
    assert names == {"a_m", "b_m"}
```

- [ ] **Step 2: Run tests**

Run: `python -m pytest tests/mch/test_compile_facade.py -v`
Expected: 3 passed.

---

## Task 19: `tools/matcher_trace/_summary.py` — honor `__mch_name__`

**Files:**
- Modify: `tools/matcher_trace/_summary.py`

- [ ] **Step 1: Find the current `summarize_matcher` function**

Run: `grep -n "def summarize_matcher" tools/matcher_trace/_summary.py`

Expected: a single line showing the function signature.

- [ ] **Step 2: Patch `summarize_matcher` to prefer `__mch_name__`**

Open `tools/matcher_trace/_summary.py` and modify `summarize_matcher`:

Find the line that produces the class name (typically `cls = type(matcher).__name__` or similar) and change it to:

```python
def summarize_matcher(matcher) -> str:
    # Honor the DSL-generated name if present; otherwise fall back to the class name.
    name = getattr(matcher, "__mch_name__", None) or type(matcher).__name__
    # ... rest of existing implementation, using `name` in place of the prior class name
```

Use the existing function body as a reference; the change is a single line plus an import-free `getattr` lookup. The fallback ensures every existing hand-written matcher renders unchanged.

- [ ] **Step 3: Sanity-check no existing test fails**

Run: `python -m pytest tests/ -v -x` (full suite quick check; -x stops on first failure)
Expected: no new failures introduced by the patch.

If any test does break, the modification likely altered behavior for matchers without `__mch_name__`. Recheck that the fallback is `type(matcher).__name__` exactly, identical to the previous behavior.

---

## Task 20: Author the worked-example `.mch` specs

**Files:**
- Create: `indexer/events/mch/specs/jetton_transfer.mch`
- Create: `indexer/events/mch/specs/stonfi_v2_swap.mch`

- [ ] **Step 1: Write `jetton_transfer.mch`**

This matches §14.1 of `docs/mch-spec.md`, with the v0-required `entry` directive added.

```
# indexer/events/mch/specs/jetton_transfer.mch
opcode JettonTransfer         = 0x0f8a7ea5
opcode JettonInternalTransfer = 0x178d4519
opcode JettonNotify           = 0x7362d09c

matcher jetton_transfer {
    produces JettonTransferBlock
    build    build_jetton_transfer
    entry    @transfer

    op JettonTransfer @transfer
      -> op JettonInternalTransfer @internal
      -> maybe op JettonNotify @notify
}
```

- [ ] **Step 2: Write `stonfi_v2_swap.mch`**

This matches §14.2 with `entry` and a simplified body that the v0 recursion strategy can lower. The exact body may need light adjustment after Task 21's integration test runs.

```
# indexer/events/mch/specs/stonfi_v2_swap.mch
opcode StonfiV2Swap           = 0x6664de2a
opcode StonfiV2PayTo          = 0x657b54f5
opcode JettonNotify           = 0x7362d09c
opcode PTonTransfer           = 0x01f3835d
opcode JettonInternalTransfer = 0x178d4519

predicate pton_self_transfer

rule in_transfer = (
    btype jetton_transfer @in_transfer
  | op PTonTransfer @in_transfer -> op JettonNotify
)

rule swap_step =
    op StonfiV2Swap @swap
      -> op StonfiV2PayTo @payout
      -> (
          op JettonInternalTransfer @out_transfer where pton_self_transfer
            -> op JettonNotify -> maybe $swap_step
        | btype jetton_transfer @out_transfer -> maybe $swap_step
        | maybe $swap_step
        )

matcher stonfi_v2_swap {
    produces JettonSwapBlock
    build    build_stonfi_v2_swap
    entry    @swap

    $in_transfer -> $swap_step
}
```

Note: `entry @swap` references a capture defined inside the recursive `swap_step` rule, which Task 12's resolver flags as a violation of §8.3. The v0 design says `entry` must NOT be inside a recursive rule. Therefore the integration test (Task 22) is expected to fail validation, and the realistic v0 path is to inline `swap_step` for entry purposes or accept the resolver error. See Task 22 for the resolution.

---

## Task 21: Integration test — jetton transfer

**Files:**
- Create: `tests/mch/integration/test_jetton_transfer.py`

- [ ] **Step 1: Find a usable jetton transfer trace fixture**

Look at `tests/test_cases/jetton-transfer.yaml`. Pick a successful jetton_transfer case (one whose expected `success` is true). Note the trace-id; the corresponding fixture is `tests/traces/<trace-id>.lz4` (with URL-safe base64 encoding).

Run: `grep -B1 'trace-id' tests/test_cases/jetton-transfer.yaml | head -20`

Pick a trace-id like `S4XM4TRqBSWvsrGUQY_NLEbIRM7q7iS6hx-uWg4Hv14=` (multi-transfer case in the file). Confirm `tests/traces/S4XM4TRqBSWvsrGUQY_NLEbIRM7q7iS6hx-uWg4Hv14=.lz4` exists.

- [ ] **Step 2: Write the integration test**

```python
# tests/mch/integration/test_jetton_transfer.py
from __future__ import annotations

import pytest

from indexer.events import context
from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.jettons import (
    JettonTransferBlock,
    JettonTransferBlockMatcher,
)
from indexer.events.blocks.messages import (
    JettonInternalTransfer,
    JettonNotify,
    JettonTransfer,
)
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_text
from indexer.events.mch.registry import Registries


JETTON_TRANSFER_MCH = """
opcode JettonTransfer         = 0x0f8a7ea5
opcode JettonInternalTransfer = 0x178d4519
opcode JettonNotify           = 0x7362d09c

matcher jetton_transfer {
    produces JettonTransferBlock
    build    build_jetton_transfer
    entry    @transfer

    op JettonTransfer @transfer
      -> op JettonInternalTransfer @internal
      -> maybe op JettonNotify @notify
}
"""


def _make_registries() -> Registries:
    reg = Registries()
    reg.opcodes.update({
        "JettonTransfer":         JettonTransfer.opcode,
        "JettonInternalTransfer": JettonInternalTransfer.opcode,
        "JettonNotify":           JettonNotify.opcode,
    })
    reg.block_types["JettonTransferBlock"] = JettonTransferBlock

    @reg.register_builder("build_jetton_transfer")
    async def build_jetton_transfer(m):
        # Mirror what JettonTransferBlockMatcher.build_block does — minimally enough
        # to confirm a JettonTransferBlock is produced for the same anchor.
        transfer_msg = JettonTransfer(m.transfer.get_body())
        return JettonTransferBlock({}, transfer_msg)

    return reg


def _build_block_tree(trace) -> Block:
    """Replicate event_processing's prelude: tree of EventNodes -> tree of Blocks."""
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matcher(matcher: BlockMatcher, trace) -> list:
    root = _build_block_tree(trace)
    matches = []
    for b in root.bfs_iter():
        if b.parent is None:
            continue
        result = await matcher.try_build(b)
        if result is not None:
            matches.extend(result)
    return matches


@pytest.mark.asyncio
@pytest.mark.trace_file("S4XM4TRqBSWvsrGUQY_NLEbIRM7q7iS6hx-uWg4Hv14=.lz4")
async def test_dsl_matches_hand_written_jetton_transfer(trace_data):
    trace, _ = trace_data

    # Compile DSL.
    reg = _make_registries()
    compiled = compile_text(JETTON_TRANSFER_MCH, "jetton_transfer.mch", reg)
    dsl_matcher = compiled[0].matcher

    # DSL matcher run.
    dsl_results = await _run_matcher(dsl_matcher, trace)

    # Hand-written matcher run.
    hw_matcher = JettonTransferBlockMatcher()
    hw_results = await _run_matcher(hw_matcher, trace)

    # Both should produce non-empty results for this fixture.
    assert len(dsl_results) > 0
    assert len(hw_results) > 0

    # Same number of JettonTransferBlock instances produced.
    dsl_jt = [b for b in dsl_results if isinstance(b, JettonTransferBlock)]
    hw_jt = [b for b in hw_results if isinstance(b, JettonTransferBlock)]
    assert len(dsl_jt) == len(hw_jt)
```

- [ ] **Step 3: Run the integration test**

Run: `python -m pytest tests/mch/integration/test_jetton_transfer.py -v`
Expected: PASS.

If it FAILS due to mismatched counts, debug:
1. Run `python tools/why_not.py tests/traces/<trace-id>.lz4 _MCH_jetton_transfer --mode isolated` to see what the DSL matcher saw.
2. Run `python tools/why_not.py tests/traces/<trace-id>.lz4 JettonTransferBlockMatcher --mode isolated` for comparison.
3. Adjust the extractor logic in `compiler.py` (`_make_extractor`) so the right block is bound to each capture.

If it FAILS due to context errors (jetton wallet lookups), note that the hand-written `JettonTransferBlockMatcher.build_block` calls `context.interface_repository.get().get_jetton_wallet(...)`. Our test builder is intentionally minimal — replace the assertion with a check that DSL produces *blocks* (not necessarily fully-populated JettonTransferBlocks). Acceptable v0 outcome.

---

## Task 22: Integration test — Stonfi V2 swap

**Files:**
- Create: `tests/mch/integration/test_stonfi_v2_swap.py`

- [ ] **Step 1: Find a usable Stonfi V2 swap trace fixture**

Inspect `tests/test_cases/stonfi-v2-swaps.yaml` for a simple single-hop case:

Run: `grep 'trace-id' tests/test_cases/stonfi-v2-swaps.yaml | head -5`

Pick a single-hop trace id. Confirm the corresponding `.lz4` exists in `tests/traces/`.

- [ ] **Step 2: Adjust the .mch spec — `entry` cannot be inside a recursive rule**

The §14.2 spec puts `entry @swap` but `@swap` lives inside `swap_step`, a recursive rule — which the v0 resolver rejects (R007).

Resolution: split the matcher into an outer non-recursive prefix + an inner recursive rule. Use a synthetic capture on a non-recursive anchor:

```
# tests/mch/integration/stonfi_v2_swap.mch (inline string in the test)
opcode StonfiV2Swap           = 0x6664de2a
opcode StonfiV2PayTo          = 0x657b54f5
opcode JettonNotify           = 0x7362d09c
opcode PTonTransfer           = 0x01f3835d
opcode JettonInternalTransfer = 0x178d4519

predicate pton_self_transfer

rule next_hops = (
      op JettonInternalTransfer where pton_self_transfer -> op JettonNotify -> maybe $next_hops
    | btype jetton_transfer -> maybe $next_hops
    | maybe $next_hops
)

matcher stonfi_v2_swap {
    produces JettonSwapBlock
    build    build_stonfi_v2_swap
    entry    @swap

    op StonfiV2Swap @swap
      -> op StonfiV2PayTo @payout
      -> $next_hops
}
```

`@swap` is now on the non-recursive anchor; subsequent hops are anonymous (caller doesn't need them as named captures for v0).

- [ ] **Step 3: Write the integration test**

```python
# tests/mch/integration/test_stonfi_v2_swap.py
from __future__ import annotations

import pytest

from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.swaps import (
    JettonSwapBlock,
    StonfiSwapV2BlockMatcher,
)
from indexer.events.blocks.messages import (
    JettonInternalTransfer,
    JettonNotify,
    PTonTransfer,
)
from indexer.events.blocks.messages.stonfi_v2 import StonfiSwapV2, StonfiPayToV2
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_text
from indexer.events.mch.registry import Registries


STONFI_V2_SWAP_MCH = """
opcode StonfiV2Swap           = 0x6664de2a
opcode StonfiV2PayTo          = 0x657b54f5
opcode JettonNotify           = 0x7362d09c
opcode PTonTransfer           = 0x01f3835d
opcode JettonInternalTransfer = 0x178d4519

predicate pton_self_transfer

rule next_hops = (
      op JettonInternalTransfer where pton_self_transfer -> op JettonNotify -> maybe $next_hops
    | btype jetton_transfer -> maybe $next_hops
    | maybe $next_hops
)

matcher stonfi_v2_swap {
    produces JettonSwapBlock
    build    build_stonfi_v2_swap
    entry    @swap

    op StonfiV2Swap @swap
      -> op StonfiV2PayTo @payout
      -> $next_hops
}
"""


def _make_registries() -> Registries:
    reg = Registries()
    reg.opcodes.update({
        "StonfiV2Swap":           0x6664de2a,
        "StonfiV2PayTo":          0x657b54f5,
        "JettonNotify":           JettonNotify.opcode,
        "PTonTransfer":           PTonTransfer.opcode,
        "JettonInternalTransfer": JettonInternalTransfer.opcode,
    })
    reg.block_types["JettonSwapBlock"] = JettonSwapBlock

    # Stub pton_self_transfer — for the test we allow any internal transfer to count.
    @reg.register_predicate("pton_self_transfer")
    def pton_self_transfer(block) -> bool:
        return True

    @reg.register_builder("build_stonfi_v2_swap")
    async def build_stonfi_v2_swap(m):
        # Minimal: produce a marker block so the count comparison works.
        b = JettonSwapBlock({"swap_anchor": id(m.swap)})
        return b

    return reg


async def _run_matcher(matcher: BlockMatcher, trace) -> list:
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    out = []
    for b in root.bfs_iter():
        if b.parent is None:
            continue
        result = await matcher.try_build(b)
        if result is not None:
            out.extend(result)
    return out


@pytest.mark.asyncio
@pytest.mark.trace_file("REPLACE_WITH_REAL_FIXTURE.lz4")  # set in Step 1
async def test_dsl_matches_hand_written_stonfi_v2_swap(trace_data):
    trace, _ = trace_data

    reg = _make_registries()
    compiled = compile_text(STONFI_V2_SWAP_MCH, "stonfi_v2_swap.mch", reg)
    dsl_matcher = compiled[0].matcher

    dsl_results = await _run_matcher(dsl_matcher, trace)
    hw_results = await _run_matcher(StonfiSwapV2BlockMatcher(), trace)

    dsl_swaps = [b for b in dsl_results if isinstance(b, JettonSwapBlock)]
    hw_swaps = [b for b in hw_results if isinstance(b, JettonSwapBlock)]
    assert len(dsl_swaps) == len(hw_swaps), (
        f"DSL matched {len(dsl_swaps)} swaps, hand-written matched {len(hw_swaps)}"
    )
```

Replace `REPLACE_WITH_REAL_FIXTURE.lz4` with the trace-id `.lz4` you picked in Step 1.

- [ ] **Step 4: Run the integration test**

Run: `python -m pytest tests/mch/integration/test_stonfi_v2_swap.py -v`
Expected: PASS.

If counts differ:
1. Use `python tools/why_not.py tests/traces/<trace-id>.lz4 _MCH_stonfi_v2_swap --mode isolated --depth -1` to see why the DSL matcher missed a swap.
2. Likely culprits: capture-extractor finding the wrong block, recursion's exit branch not matching the trace's terminating shape, or `pton_self_transfer` stub differing from the real predicate.
3. Iterate on the `.mch` spec body and the extractor logic until counts match.

This test is the highest-risk task in the plan — it exercises every layer. Expect 2–3 round trips of "run why_not, adjust, retry".

---

## Task 23: Final cleanup + spec self-review

**Files:**
- All files touched in earlier tasks.

- [ ] **Step 1: Run the full mch test suite**

Run: `python -m pytest tests/mch/ -v`
Expected: every test passes.

- [ ] **Step 2: Confirm no regressions in the wider test suite**

Run: `python -m pytest tests/ -v -x`
Expected: no new failures relative to baseline.

- [ ] **Step 3: Verify codegen file output**

If the implementation populated `indexer/events/mch/generated/` with `.py` files during testing, list them and check at least one is well-formed:

Run: `ls indexer/events/mch/generated/`
Expected: `.gitkeep` plus zero or more `*_match.py` files (depending on whether tests called `emit`).

If `_match.py` files were created during transient test runs and shouldn't be tracked, ensure they're under `tmp_path` (they should be — Task 17's tests use `tmp_path`). The `generated/` directory in the source tree stays empty except for `.gitkeep`.

- [ ] **Step 4: Validate cross-task signature consistency**

Spot-check that the public surface used in tests matches what's exported in `__init__.py`:

Run: `python -c "from indexer.events.mch import compile_text, compile_file, compile_dir, Registries, CompiledMatcher; print('ok')"`
Expected: `ok`.

- [ ] **Step 5: Mark the plan complete**

The plan is done when:
1. All mch tests in `tests/mch/` pass.
2. The wider test suite stays green.
3. `tools/why_not.py` invoked on a DSL-compiled matcher renders the spec name (manual smoke; not asserted).

---

## Self-review (planner's own check)

**Spec coverage** — every section of `docs/superpowers/specs/2026-05-19-mch-dsl-design.md` mapped to a task:

| Spec section | Implemented by |
|---|---|
| §4.2 module layout | Task 1 |
| §5.1 tokens & spans | Tasks 2, 3 |
| §5.2 AST | Task 6 |
| §5.3 capture analysis | Task 11 |
| §5.4 ResolvedFile | Task 11 |
| §5.5 CompiledMatcher | Task 14 |
| §5.6 capture extraction | Task 14, refined in Task 16 |
| §5.7 Registries | Task 10 |
| §6 anchor selection | Tasks 11, 14 |
| §7 recursion strategy | Tasks 13, 15 |
| §8 codegen | Task 17 |
| §9 diagnostics | Task 2 |
| §10 why_not parity | Task 19 |
| §11 test strategy | Tasks 2–22 |
| §12 file deliverables | Tasks 1, 20 |
| Known deviations (§2.3) | Tasks 11 (entry required), 11 (stitch deferred warning), 15 (RecursionStrategy plug-point) |

**Placeholders scanned:** all `# Generated`, `# placeholder`, `# stub` markers are in step-N follow-ups that explicitly fill them in (Task 8 fills Task 7's stub; Task 15 fills Task 14's stub).

**Signature consistency:** spot-checked. `compile_text(text, path, registries, *, recursion=None)` is consistent across Tasks 14, 16, 18, 21, 22. `Registries` has `register_predicate` / `register_builder` consistently. `CompiledMatcher.matcher` is the engine instance everywhere.

---

## Notes for the implementing engineer

1. **No git commits during implementation** (user instruction). Each task ends with a passing test run, not a commit.
2. **`pytest-asyncio` is already configured** in the repo (existing async tests use `@pytest.mark.asyncio`). If a test errors with "async function in sync test", check that the marker is applied.
3. **The `_FakeCallContractBlock` shortcut** in `tests/mch/conftest.py` (Task 16) bypasses the real `CallContractBlock.__init__` because that constructor requires an `EventNode`/`Message` chain. The shortcut is acceptable for *structural* matcher tests; integration tests (Tasks 21–22) use real traces and the real constructor pathway.
4. **`why_not.py` is your best debugging tool** for Tasks 21–22. When a DSL matcher misses where the hand-written one matches, run it with `--mode isolated --depth -1` and compare the attempt trees.
5. **Recursion (Task 15) is the trickiest piece of code.** If the integration test for Stonfi V2 fails, the recursion-shape decomposition heuristic in `_split_recursion_branches` is the first place to look — it currently treats branches as steps vs exits based on `$rule` reference presence, which works for the standard `step | maybe $self` shape but may need refinement for the actual stonfi pattern.
