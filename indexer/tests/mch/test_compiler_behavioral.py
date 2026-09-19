"""Frontend compile-time diagnostics for pattern lowering (C005-C013).

T29 stage 1 retired the compiled runtime, so the behavioral match tests that
used to live here (running compiled BlockMatcher instances over synthetic block
trees) now run against the v2 IR engine in tests/mch_ir/ — capture binding,
chain/parent/children/XOR semantics, recursion, declarative build, and host
functions in tests/mch_ir/test_engine.py, test_walker.py, and
test_compiled_ports.py. What remains here is purely frontend: patterns the
compiler must reject at compile time (no runtime), plus one lowering smoke test.
"""
from __future__ import annotations

import pytest

from indexer.events.mch.compile import compile_text
from indexer.events.mch.diagnostics import CompileError
from indexer.events.mch.registry import Registries


def _registries_with(opcodes: dict[str, int], produces: str) -> Registries:
    reg = Registries()
    reg.opcodes.update(opcodes)
    return reg


def _error_codes(src: str, reg: Registries) -> list[str]:
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    return [d.code for d in exc.value.bag.items]


# ==========================================================================
# C007 — captures after a recursive rule reference in the same sequence
# ==========================================================================

def test_capture_after_recursion_rejected():
    src = """
    opcode A = 0x1
    opcode X = 0x9

    rule step = op A @a -> maybe $step

    matcher m {
        produces MB

        op A @anchor -> $step -> op X @x
        out { v: 1 }
    }
    """
    reg = _registries_with({"A": 0x1, "X": 0x9}, "MB")
    assert "C007_CAPTURE_AFTER_RECURSION" in _error_codes(src, reg)


# ==========================================================================
# C008 — a children block may only attach to a chain edge
# ==========================================================================


def test_children_block_inside_alternative_branch_rejected():
    """or { op A ; {op B; op C} } → C008 compile error."""
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3

    matcher m {
        produces MB
        op A @a -> or { op A ; {op B; op C} }
        out { v: 1 }
    }
    """
    reg = _registries_with({"A": 0x1, "B": 0x2, "C": 0x3}, "MB")
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    assert "C008_CHILDREN_BLOCK_BARE" in [d.code for d in exc.value.bag.items]


def test_children_block_as_sequence_head_rejected():
    """{op B; op C} -> op A as head of sequence → C008."""
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3

    matcher m {
        produces MB
        {op B; op C} -> op A @a
        out { v: 1 }
    }
    """
    reg = _registries_with({"A": 0x1, "B": 0x2, "C": 0x3}, "MB")
    assert _error_codes(src, reg) == ["C008_CHILDREN_BLOCK_BARE"]


def test_children_block_as_parent_target_rejected():
    """op A @a <- {op B} — a children block cannot be a parent link → C008."""
    src = """
    opcode A = 0x1
    opcode B = 0x2

    matcher m {
        produces MB
        op A @a <- { op B }
        out { v: 1 }
    }
    """
    reg = _registries_with({"A": 0x1, "B": 0x2}, "MB")
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    assert "C008_CHILDREN_BLOCK_BARE" in [d.code for d in exc.value.bag.items]


def test_children_block_rule_body_root_rejected():
    """rule r = {op A; op B} referenced from chain → C008."""
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode X = 0x9

    rule r = {op A; op B}

    matcher m {
        produces MB
        op X @x -> $r
        out { v: 1 }
    }
    """
    reg = _registries_with({"A": 0x1, "B": 0x2, "X": 0x9}, "MB")
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    assert "C008_CHILDREN_BLOCK_BARE" in [d.code for d in exc.value.bag.items]


# ==========================================================================
# An or block inside a recursive rule compiles cleanly
# ==========================================================================

def test_or_inside_recursive_rule_compiles():
    """`rule r = or { op A @a ; maybe $r }` lowers without error."""
    src = """
    opcode A = 0x1

    rule r = or { op A @a ; maybe $r }

    matcher m {
        produces MB

        op A @anchor -> $r
        out { v: 1 }
    }
    """
    reg = _registries_with({"A": 0x1}, "MB")
    compiled = compile_text(src, "test.mch", reg)
    assert compiled and compiled[0].name == "m"
