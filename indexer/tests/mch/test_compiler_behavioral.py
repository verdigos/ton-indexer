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
from indexer.events.mch.diagnostics import CompileError, Severity
from indexer.events.mch.registry import Registries


def _registries_with(opcodes: dict[str, int], builder_name: str, produces: str) -> Registries:
    reg = Registries()
    reg.opcodes.update(opcodes)
    reg.block_types[produces] = type(produces, (), {})

    @reg.register_builder(builder_name)
    async def _build(m):  # noqa: ARG001
        return None
    return reg


def _error_codes(src: str, reg: Registries) -> list[str]:
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    return [d.code for d in exc.value.bag.items if d.severity is Severity.ERROR]


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
        build    build_m
        entry    @anchor

        op A @anchor -> $step -> op X @x
    }
    """
    reg = _registries_with({"A": 0x1, "X": 0x9}, "build_m", "MB")
    assert "C007_CAPTURE_AFTER_RECURSION" in _error_codes(src, reg)


# ==========================================================================
# C010 — upstream atoms before the anchor cannot be lowered
# ==========================================================================

def test_parent_chain_in_tail_position_child_edge_anchor():
    """Anchor in tail with a CHILD incoming edge (`op X -> op A @a <- …`): the
    upstream `op X` (the anchor's parent) is not lowered — rejected with C010
    rather than silently dropped."""
    src = """
    opcode X = 0x9
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3

    matcher m {
        produces MB
        build    build_m
        entry    @a

        op X -> op A @a <- op B @b <- op C @c
    }
    """
    reg = _registries_with({"X": 0x9, "A": 0x1, "B": 0x2, "C": 0x3}, "build_m", "MB")
    assert "C010_UPSTREAM_ATOMS_UNSUPPORTED" in _error_codes(src, reg)


# ==========================================================================
# C008 — a children block may only attach to a chain edge
# ==========================================================================

def test_children_block_inside_alternative_branch_rejected():
    """(op A | {op B; op C}) → C008 compile error."""
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3

    matcher m {
        produces MB
        build    build_m
        entry    @a
        op A @a -> (op A | {op B; op C})
    }
    """
    reg = _registries_with({"A": 0x1, "B": 0x2, "C": 0x3}, "build_m", "MB")
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    assert "C008_CHILDREN_BLOCK_BARE" in [d.code for d in exc.value.bag.items]


def test_children_block_inside_bare_maybe_rejected():
    """maybe {op B; op C} as standalone → C008 compile error."""
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3

    rule r = maybe {op B; op C}

    matcher m {
        produces MB
        build    build_m
        entry    @a
        op A @a -> $r
    }
    """
    reg = _registries_with({"A": 0x1, "B": 0x2, "C": 0x3}, "build_m", "MB")
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
        build    build_m
        entry    @a
        {op B; op C} -> op A @a
    }
    """
    reg = _registries_with({"A": 0x1, "B": 0x2, "C": 0x3}, "build_m", "MB")
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
        build    build_m
        entry    @x
        op X @x -> $r
    }
    """
    reg = _registries_with({"A": 0x1, "B": 0x2, "X": 0x9}, "build_m", "MB")
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    assert "C008_CHILDREN_BLOCK_BARE" in [d.code for d in exc.value.bag.items]


# ==========================================================================
# Lowering smoke: a caret inside a recursive rule compiles cleanly
# ==========================================================================

def test_caret_inside_recursive_rule_compiles():
    """`rule r = (op A @a ^ maybe $r)` lowers without error (Phase 3 guard)."""
    src = """
    opcode A = 0x1

    rule r = (op A @a ^ maybe $r)

    matcher m {
        produces MB
        build    build_m
        entry    @anchor

        op A @anchor -> $r
    }
    """
    reg = _registries_with({"A": 0x1}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    assert compiled and compiled[0].name == "m"
