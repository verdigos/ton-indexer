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
