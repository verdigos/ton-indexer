"""Declarative build path (M4 §9) — frontend resolver diagnostics (R011-R018).

T29 stage 1 retired the compiled runtime, so the end-to-end run of declarative
matchers (parse/let/lookup/reject/failed/broken/out, runtime-error
rejection) is now exercised against the v2 IR engine in
tests/mch_ir/test_engine.py and tests/mch_ir/test_compiled_ports.py. What
remains here is the resolver's compile-time validation of build statements.
"""
from __future__ import annotations

import pytest

from indexer.events.mch.diagnostics import CompileError
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.registry import Registries
from indexer.events.mch.resolver import resolve

OP_FOO = 0xF00
OP_BAR = 0xBA7


class FakeTransfer:
    """A registered message type name — the resolver only checks existence."""

    def __init__(self, s):  # pragma: no cover - never parsed in resolver tests
        raise NotImplementedError


def _registries() -> Registries:
    reg = Registries()
    reg.opcodes.update({"Foo": OP_FOO, "Bar": OP_BAR})
    reg.block_types["FB"] = object
    reg.message_types["FakeTransfer"] = FakeTransfer
    return reg


def _codes(src: str, reg: Registries | None = None) -> list[str]:
    reg = reg or _registries()
    with pytest.raises(CompileError) as exc:
        resolve(parse(list(tokenize(src, "t.mch")), "t.mch"), reg)
    return [d.code for d in exc.value.bag.items]


_DIAG_HEAD = "matcher m {\n  produces FB\n  entry @x\n\n  op Foo @x\n"


def _diag_src(stmts: str, head: str = _DIAG_HEAD) -> str:
    return "opcode Foo = 0xF00\n" + head + stmts + "\n}"


def test_r011_parse_target_must_be_capture():
    codes = _codes(_diag_src("parse ghost as FakeTransfer\nout { v: 1 }"))
    assert "R011_PARSE_TARGET_NOT_CAPTURE" in codes


def test_r012_unknown_message_type():
    codes = _codes(_diag_src("parse x as NoSuchType\nout { v: 1 }"))
    assert "R012_UNKNOWN_MESSAGE_TYPE" in codes


def test_r013_duplicate_let_and_capture_shadowing():
    codes = _codes(_diag_src("let a = 1\nlet a = 2\nout { v: a }"))
    assert "R013_DUP_LET" in codes
    codes = _codes(_diag_src("let x = 1\nout { v: x }"))
    assert "R013_DUP_LET" in codes


def test_r014_duplicate_out_field():
    codes = _codes(_diag_src("out { v: 1, v: 2 }"))
    assert "R014_DUP_OUT_FIELD" in codes


# --- produces switch (§9.5) resolver diagnostics ---------------------------

def _switch_registries() -> Registries:
    reg = _registries()
    for bt in ("A", "B", "C"):
        reg.block_types[bt] = object
    return reg


def _switch_src(body: str, produces: str = "A | B") -> str:
    return (
        "opcode Foo = 0xF00\n"
        f"matcher m {{\n  produces {produces}\n  entry @x\n\n  op Foo @x\n"
        + body + "\n}"
    )


def test_switch_dispatch_resolves_ok():
    # A well-formed switch compiles clean (no diagnostics).
    src = _switch_src(
        "produces switch {\n"
        "  when 1 == 1 => A out { v: 1 }\n"
        "  else => B out { v: 2 }\n"
        "}"
    )
    resolve(parse(list(tokenize(src, "t.mch")), "t.mch"), _switch_registries())


def test_switch_requires_else():
    src = _switch_src("produces switch {\n  when 1 == 1 => A out { v: 1 }\n}")
    codes = _codes(src, _switch_registries())
    assert "R021_SWITCH_NO_ELSE" in codes


def test_switch_branch_after_else():
    src = _switch_src(
        "produces switch {\n"
        "  else => A out { v: 1 }\n"
        "  when 1 == 1 => B out { v: 2 }\n"
        "}"
    )
    codes = _codes(src, _switch_registries())
    assert "R019_SWITCH_BRANCH_AFTER_ELSE" in codes


def test_switch_btype_not_in_produces():
    src = _switch_src(
        "produces switch {\n"
        "  when 1 == 1 => C out { v: 1 }\n"  # C not in `produces A | B`
        "  else => A out { v: 2 }\n"
        "}"
    )
    codes = _codes(src, _switch_registries())
    assert "R020_SWITCH_BTYPE_NOT_DECLARED" in codes


def test_switch_and_out_are_mutually_exclusive():
    src = _switch_src(
        "out { v: 1 }\n"
        "produces switch {\n  when 1 == 1 => A out { v: 1 }\n  else => B out { v: 2 }\n}"
    )
    codes = _codes(src, _switch_registries())
    assert "R017_MULTIPLE_OUT" in codes


def test_r015_build_directive_plus_statements_is_ambiguous():
    src = (
        "opcode Foo = 0xF00\n"
        "matcher m {\n  produces FB\n  build b\n  entry @x\n\n"
        "  op Foo @x\n  out { v: 1 }\n}"
    )
    assert "R015_BUILD_AMBIGUOUS" in _codes(src)


def test_r016_build_statements_without_out():
    codes = _codes(_diag_src("let a = 1"))
    assert "R016_BUILD_NO_OUT" in codes


def test_r017_multiple_out_blocks():
    codes = _codes(_diag_src("out { v: 1 }\nout { w: 2 }"))
    assert "R017_MULTIPLE_OUT" in codes


def test_declarative_matcher_resolves_clean():
    # Control: the declarative form itself carries no errors.
    src = _diag_src(
        "parse x as FakeTransfer\n"
        "let amt = x.body.amount\n"
        "reject when amt == null\n"
        "out { amount: amt }"
    )
    rf = resolve(parse(list(tokenize(src, "t.mch")), "t.mch"), _registries())
    assert rf.matchers[0].decl.build is None
    assert len(rf.matchers[0].build_stmts) == 4
