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


from indexer.events.blocks.basic_matchers import RecursiveMatcher


def test_recursive_rule_lowers_to_recursive_matcher():
    src = """
    opcode A      = 0x1
    opcode B      = 0x2
    opcode Anchor = 0x3

    rule swap_step =
        op A @step -> ( maybe $swap_step | op B @term )

    matcher m {
        produces MB
        build    build_m
        entry    @anchor

        op Anchor @anchor -> $swap_step
    }
    """
    reg = _registries_with_opcodes(A=0x1, B=0x2, Anchor=0x3)
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


def test_multi_hop_parent_chain_lowers_to_chain():
    """Phase 4: `op A @a <- op B <- op C` lowers to a chain of parent_matchers.
    Previously rejected via C006; now compiles cleanly."""
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3

    matcher m {
        produces MB
        build    build_m
        entry    @a

        op A @a <- op B @b <- op C @c
    }
    """
    reg = _registries_with_opcodes(A=0x1, B=0x2, C=0x3)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher
    assert m.parent_matcher is not None, "anchor must have parent_matcher set"
    assert m.parent_matcher.parent_matcher is not None, "multi-hop chain must form"
    # Leaf opcode should be C (0x3).
    leaf = m.parent_matcher.parent_matcher
    assert getattr(leaf, "opcode", None) == 0x3, (
        f"leaf parent_matcher opcode = {getattr(leaf, 'opcode', None)!r}, expected 0x3"
    )


def test_indirect_recursion_rejected():
    """Mutual recursion across two rules is not supported in v0; compiler errors."""
    from indexer.events.mch.diagnostics import CompileError, Severity
    src = """
    opcode A      = 0x1
    opcode B      = 0x2
    opcode Anchor = 0x3

    rule r_a = op A @a -> maybe $r_b
    rule r_b = op B @b -> maybe $r_a

    matcher m {
        produces MB
        build    build_m
        entry    @anchor

        op Anchor @anchor -> $r_a
    }
    """
    reg = _registries_with_opcodes(A=0x1, B=0x2, Anchor=0x3)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    codes = [d.code for d in exc.value.bag.items if d.severity is Severity.ERROR]
    assert "C005_INDIRECT_RECURSION_UNSUPPORTED" in codes, codes


# ==========================================================================
# Phase 7 — XOR separator structural tests
# ==========================================================================

def test_caret_separator_lowers_to_exclusive_or():
    """op X @x -> (op A ^ op B): child_matcher is ExclusiveOrMatcher."""
    from indexer.events.blocks.basic_matchers import ExclusiveOrMatcher
    src = """
    opcode X = 0x9
    opcode A = 0x1
    opcode B = 0x2

    matcher m {
        produces MB
        build    build_m
        entry    @x

        op X @x -> (op A ^ op B)
    }
    """
    reg = _registries_with_opcodes(X=0x9, A=0x1, B=0x2)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    compiled = compile_text(src, "test.mch", reg)
    child = compiled[0].matcher.child_matcher
    assert isinstance(child, ExclusiveOrMatcher), f"expected ExclusiveOrMatcher, got {type(child).__name__}"


def test_pipe_concrete_distinct_lowers_to_exclusive_or():
    """op X @x -> (op A | op B): heuristic fires → child_matcher is ExclusiveOrMatcher."""
    from indexer.events.blocks.basic_matchers import ExclusiveOrMatcher
    src = """
    opcode X = 0x9
    opcode A = 0x1
    opcode B = 0x2

    matcher m {
        produces MB
        build    build_m
        entry    @x

        op X @x -> (op A | op B)
    }
    """
    reg = _registries_with_opcodes(X=0x9, A=0x1, B=0x2)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    compiled = compile_text(src, "test.mch", reg)
    child = compiled[0].matcher.child_matcher
    assert isinstance(child, ExclusiveOrMatcher), f"expected ExclusiveOrMatcher, got {type(child).__name__}"


def test_pipe_with_predicate_lowers_to_or():
    """op X @x -> (op A | op A where p): heuristic blocked by predicate → child_matcher is plain OrMatcher."""
    from indexer.events.blocks.basic_matchers import ExclusiveOrMatcher
    src = """
    opcode X = 0x9
    opcode A = 0x1
    predicate p

    matcher m {
        produces MB
        build    build_m
        entry    @x

        op X @x -> (op A | op A where p)
    }
    """
    reg = _registries_with_opcodes(X=0x9, A=0x1)
    reg.predicates["p"] = lambda b: True
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    compiled = compile_text(src, "test.mch", reg)
    child = compiled[0].matcher.child_matcher
    assert isinstance(child, OrMatcher) and not isinstance(child, ExclusiveOrMatcher), (
        f"expected plain OrMatcher, got {type(child).__name__}"
    )


def test_pipe_nested_lowers_to_or():
    """op X @x -> (op A | (op B -> op C)): heuristic blocked by Sequence → child_matcher is plain OrMatcher."""
    from indexer.events.blocks.basic_matchers import ExclusiveOrMatcher
    src = """
    opcode X = 0x9
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3

    matcher m {
        produces MB
        build    build_m
        entry    @x

        op X @x -> (op A | (op B -> op C))
    }
    """
    reg = _registries_with_opcodes(X=0x9, A=0x1, B=0x2, C=0x3)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    compiled = compile_text(src, "test.mch", reg)
    child = compiled[0].matcher.child_matcher
    assert isinstance(child, OrMatcher) and not isinstance(child, ExclusiveOrMatcher)


def test_caret_with_predicate_still_exclusive_or():
    """op X @x -> (op A ^ op A where p): explicit ^ → child_matcher is ExclusiveOrMatcher."""
    from indexer.events.blocks.basic_matchers import ExclusiveOrMatcher
    src = """
    opcode X = 0x9
    opcode A = 0x1
    predicate p

    matcher m {
        produces MB
        build    build_m
        entry    @x

        op X @x -> (op A ^ op A where p)
    }
    """
    reg = _registries_with_opcodes(X=0x9, A=0x1)
    reg.predicates["p"] = lambda b: True
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    compiled = compile_text(src, "test.mch", reg)
    child = compiled[0].matcher.child_matcher
    assert isinstance(child, ExclusiveOrMatcher)


def test_nested_caret_outer_pipe_inner():
    """op X @x -> (op A ^ (op B | op C where p)): outer is ExclusiveOrMatcher; inner is plain OrMatcher (predicate blocks heuristic)."""
    from indexer.events.blocks.basic_matchers import ExclusiveOrMatcher
    src = """
    opcode X = 0x9
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    predicate p

    matcher m {
        produces MB
        build    build_m
        entry    @x

        op X @x -> (op A ^ (op B | op C where p))
    }
    """
    reg = _registries_with_opcodes(X=0x9, A=0x1, B=0x2, C=0x3)
    reg.predicates["p"] = lambda b: True
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    compiled = compile_text(src, "test.mch", reg)
    child = compiled[0].matcher.child_matcher
    assert isinstance(child, ExclusiveOrMatcher), f"outer child must be ExclusiveOrMatcher, got {type(child).__name__}"
    inner = child.matchers[1]  # (op B | op C where p) branch — predicate blocks heuristic
    assert isinstance(inner, OrMatcher) and not isinstance(inner, ExclusiveOrMatcher), (
        f"inner must be plain OrMatcher, got {type(inner).__name__}"
    )


def test_caret_redundant_heads_warns(monkeypatch):
    """op X @x -> (op A ^ op A): W202 emitted; child_matcher is ExclusiveOrMatcher."""
    from indexer.events.blocks.basic_matchers import ExclusiveOrMatcher
    from indexer.events.mch import diagnostics as diag_module
    warnings_captured = []

    original_warning = diag_module.DiagnosticBag.warning
    def _patched_warning(self, code, message, span, suggestion=None):
        warnings_captured.append(code)
        return original_warning(self, code, message, span, suggestion)
    monkeypatch.setattr(diag_module.DiagnosticBag, "warning", _patched_warning)

    src = """
    opcode X = 0x9
    opcode A = 0x1

    matcher m {
        produces MB
        build    build_m
        entry    @x

        op X @x -> (op A ^ op A)
    }
    """
    reg = _registries_with_opcodes(X=0x9, A=0x1)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    compiled = compile_text(src, "test.mch", reg)
    assert "W202_EXCLUSIVE_OR_REDUNDANT_HEADS" in warnings_captured, (
        f"expected W202 warning, got: {warnings_captured}"
    )
    child = compiled[0].matcher.child_matcher
    assert isinstance(child, ExclusiveOrMatcher)
