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


# ==========================================================================
# T12 — Mixed-direction chain conformance (spec §6.4, §6.5)
#
# Each test pins the EXACT child_matcher / parent_matcher / children_matchers
# wiring the compiler must produce for a tricky chain lowering.
# ==========================================================================


def _compile_one(src: str, **opcodes: int):
    reg = _registries_with_opcodes(**opcodes)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    return compile_text(src, "test.mch", reg)[0].matcher


def test_t12_shape1_anchor_headed_parent_edge():
    """`op A @a <- op B @b` (entry @a) — B becomes A's parent_matcher; nothing else."""
    m = _compile_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        matcher m {
            produces MB
            build    build_m
            entry    @a
            op A @a <- op B @b
        }
        """,
        A=0x1, B=0x2,
    )
    assert isinstance(m, ContractMatcher) and m.opcode == 0x1
    assert m.child_matcher is None
    assert m.children_matchers is None
    assert isinstance(m.parent_matcher, ContractMatcher)
    assert m.parent_matcher.opcode == 0x2
    assert m.parent_matcher.parent_matcher is None


def test_t12_shape2_multihop_parent_chain():
    """`op A <- op B <- op C` — A.parent=B, B.parent=C (multi-hop; Phase 4)."""
    m = _compile_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        matcher m {
            produces MB
            build    build_m
            entry    @a
            op A @a <- op B @b <- op C @c
        }
        """,
        A=0x1, B=0x2, C=0x3,
    )
    assert m.child_matcher is None
    assert m.children_matchers is None
    b = m.parent_matcher
    assert isinstance(b, ContractMatcher) and b.opcode == 0x2
    c = b.parent_matcher
    assert isinstance(c, ContractMatcher) and c.opcode == 0x3
    assert c.parent_matcher is None


def test_t12_shape3_children_after_parent_edge_attach_to_parent():
    """`op A <- op B -> {C; D}` — CB attaches to B (the parent matcher), NOT to
    anchor A (spec §6.5 attach-to-left after a parent edge)."""
    m = _compile_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        opcode D = 0x4
        matcher m {
            produces MB
            build    build_m
            entry    @a
            op A @a <- op B @b -> {op C @c; op D @d}
        }
        """,
        A=0x1, B=0x2, C=0x3, D=0x4,
    )
    # Anchor A owns neither child nor children — the CB routed to B.
    assert m.child_matcher is None
    assert m.children_matchers is None, "children block must NOT attach to anchor A"
    b = m.parent_matcher
    assert isinstance(b, ContractMatcher) and b.opcode == 0x2
    assert b.parent_matcher is None
    assert b.children_matchers is not None
    assert [cm.opcode for cm in b.children_matchers] == [0x3, 0x4]


def test_t12_shape4_mid_chain_children_attach_to_left():
    """`op A -> op B -> {C; D}` — mid-chain CB attaches to B (Phase 5)."""
    m = _compile_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        opcode D = 0x4
        matcher m {
            produces MB
            build    build_m
            entry    @a
            op A @a -> op B @b -> {op C @c; op D @d}
        }
        """,
        A=0x1, B=0x2, C=0x3, D=0x4,
    )
    assert m.parent_matcher is None
    assert m.children_matchers is None, "children block must attach to B, not anchor A"
    b = m.child_matcher
    assert isinstance(b, ContractMatcher) and b.opcode == 0x2
    assert b.children_matchers is not None
    assert [cm.opcode for cm in b.children_matchers] == [0x3, 0x4]


def test_t12_shape5_children_block_does_not_advance_cursor():
    """`op A -> {B; C} -> op D` — A has BOTH children_matchers=[B, C] AND
    child_matcher=D. A children block does not advance the chain cursor."""
    m = _compile_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        opcode D = 0x4
        matcher m {
            produces MB
            build    build_m
            entry    @a
            op A @a -> {op B @b; op C @c} -> op D @d
        }
        """,
        A=0x1, B=0x2, C=0x3, D=0x4,
    )
    assert m.parent_matcher is None
    assert m.children_matchers is not None
    assert [cm.opcode for cm in m.children_matchers] == [0x2, 0x3]
    assert isinstance(m.child_matcher, ContractMatcher)
    assert m.child_matcher.opcode == 0x4
    # D advances the chain but the CB did not — D has no further child.
    assert m.child_matcher.child_matcher is None


# ==========================================================================
# T10 — Opcode-set anchors (spec §8.3 M2)
# ==========================================================================


def test_t10_opcode_set_anchor_membership_test_and_chain_wiring(build_block_tree):
    """jetton_mint shape: `(op A @m ^ op B @m) -> maybe op N @n` — the compiled
    matcher's test_self is set membership over both anchor opcodes, and the
    chain edge off the group wires as the anchor's child_matcher."""
    from tests.mch.conftest import BlockSpec

    m = _compile_one(
        """
        opcode JettonMint = 0x1
        opcode MinterJettonMint = 0x2
        opcode JettonNotify = 0x3
        matcher jetton_mint {
            produces MB
            build    build_m
            entry    @m
            (op JettonMint @m ^ op MinterJettonMint @m) -> maybe op JettonNotify @n
        }
        """,
        JettonMint=0x1, MinterJettonMint=0x2, JettonNotify=0x3,
    )
    assert isinstance(m, GenericMatcher)
    assert type(m).__name__ == "_MCH_jetton_mint"
    # Set membership: fires for both anchor opcodes, not for a third.
    assert m.test_self(build_block_tree(BlockSpec(opcode=0x1))) is True
    assert m.test_self(build_block_tree(BlockSpec(opcode=0x2))) is True
    assert m.test_self(build_block_tree(BlockSpec(opcode=0x99))) is False
    # A non-CallContractBlock never satisfies an opcode-only set.
    assert m.test_self(build_block_tree(BlockSpec(btype="jetton_transfer"))) is False
    # Chain wiring off the group: single optional child, no parent/children.
    assert isinstance(m.child_matcher, ContractMatcher)
    assert m.child_matcher.opcode == 0x3
    assert m.child_matcher.optional is True
    assert m.parent_matcher is None
    assert m.children_matchers is None


def test_t10_mixed_op_and_btype_set_anchor(build_block_tree):
    """Branches mixing op and btype heads: membership is opcode-set OR btype-set."""
    from tests.mch.conftest import BlockSpec

    m = _compile_one(
        """
        opcode A = 0x1
        matcher m {
            produces MB
            build    build_m
            entry    @x
            (op A @x ^ btype jetton_transfer @x)
        }
        """,
        A=0x1,
    )
    assert m.test_self(build_block_tree(BlockSpec(opcode=0x1))) is True
    assert m.test_self(build_block_tree(BlockSpec(btype="jetton_transfer"))) is True
    assert m.test_self(build_block_tree(BlockSpec(opcode=0x99))) is False
    assert m.test_self(build_block_tree(BlockSpec(btype="other"))) is False


# ==========================================================================
# R09 — Multi-kind anchors + per-branch `where` (spec §8.3 [M2R, R09])
# ==========================================================================


def _compile_one_with_pred(src: str, pred, pred_name: str = "has_child", **opcodes: int):
    reg = _registries_with_opcodes(**opcodes)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    reg.predicates[pred_name] = pred
    return compile_text(src, "test.mch", reg)[0].matcher


def test_r09_per_branch_where_gates_membership(build_block_tree):
    """`(op A @x ^ op B @x where has_child)` — membership over {A, B}, but the
    B branch only anchors when its `where` predicate passes. A has no where."""
    from tests.mch.conftest import BlockSpec

    m = _compile_one_with_pred(
        """
        opcode A = 0x1
        opcode B = 0x2
        matcher m {
            produces MB
            build    build_m
            entry    @x
            (op A @x ^ op B @x where has_child)
        }
        """,
        pred=lambda b: bool(getattr(b, "next_blocks", None)),
        A=0x1, B=0x2,
    )
    # A branch: no where, always fires on membership.
    assert m.test_self(build_block_tree(BlockSpec(opcode=0x1))) is True
    # B branch with a child: where passes -> fires.
    assert m.test_self(
        build_block_tree(BlockSpec(opcode=0x2, children=[BlockSpec(opcode=0x9)]))
    ) is True
    # B branch without a child: head matches but where fails -> no anchor.
    assert m.test_self(build_block_tree(BlockSpec(opcode=0x2))) is False
    # Off-membership opcode never fires.
    assert m.test_self(build_block_tree(BlockSpec(opcode=0x99))) is False


def test_r09_mixed_op_btype_with_where(build_block_tree):
    """`(op A @x where has_child ^ btype jetton_transfer @x)` — mixed kinds AND a
    per-branch where: union membership {op A, btype jetton_transfer}, the op
    branch gated by `has_child`, the btype branch ungated."""
    from tests.mch.conftest import BlockSpec

    m = _compile_one_with_pred(
        """
        opcode A = 0x1
        matcher m {
            produces MB
            build    build_m
            entry    @x
            (op A @x where has_child ^ btype jetton_transfer @x)
        }
        """,
        pred=lambda b: bool(getattr(b, "next_blocks", None)),
        A=0x1,
    )
    assert m.test_self(
        build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x9)]))
    ) is True
    assert m.test_self(build_block_tree(BlockSpec(opcode=0x1))) is False  # where fails
    assert m.test_self(build_block_tree(BlockSpec(btype="jetton_transfer"))) is True
    assert m.test_self(build_block_tree(BlockSpec(btype="other"))) is False


def test_t12_shape6_child_then_parent_chain_rejected():
    """`op A -> op B <- op C` — degenerate: B matched as A's child has
    previous_block==A, so a parent_matcher C on B would test the anchor block and
    never match. Rejected at compile time with C009 (spec §6.4) rather than
    silently dropping the `<- op C` segment."""
    from indexer.events.mch.diagnostics import CompileError, Severity

    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    matcher m {
        produces MB
        build    build_m
        entry    @a
        op A @a -> op B @b <- op C @c
    }
    """
    reg = _registries_with_opcodes(A=0x1, B=0x2, C=0x3)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    codes = [d.code for d in exc.value.bag.items if d.severity is Severity.ERROR]
    assert "C009_MIXED_CHILD_PARENT_CHAIN_UNSUPPORTED" in codes, codes


def test_t15_shape7_children_block_then_parent_edge():
    """`op A -> {B; maybe C} <- op P` — a ChildrenBlock does not advance the
    chain cursor (spec §6.5), so the parent edge after it still belongs to the
    anchor: A gets BOTH children_matchers=[B, C(optional)] AND parent_matcher=P.
    This is the dedust-swap shape (T15); previously mis-rejected with C009."""
    m = _compile_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        opcode P = 0x4
        matcher m {
            produces MB
            build    build_m
            entry    @a
            op A @a -> {op B @b; maybe op C @c} <- op P @p
        }
        """,
        A=0x1, B=0x2, C=0x3, P=0x4,
    )
    assert m.child_matcher is None
    assert m.children_matchers is not None
    assert [cm.opcode for cm in m.children_matchers] == [0x2, 0x3]
    assert [cm.optional for cm in m.children_matchers] == [False, True]
    p = m.parent_matcher
    assert isinstance(p, ContractMatcher) and p.opcode == 0x4
    assert p.children_matchers is None, "parent must not inherit the anchor's CB"


# ==========================================================================
# T04 — silent-drop lowering fixes (spec §6.4/§6.5, §12.1)
# ==========================================================================


def test_t04_flat_parent_chain_children_then_parent_edge():
    """`@a <- op X -> {op D} <- op Y` (flat): the trailing `<- Y` parent edge
    after a children block is no longer dropped. X gets BOTH children_matchers=[D]
    AND parent_matcher=Y — lowering identical to the grouped form
    `<- ( op X -> {op D} <- op Y )` (T04 Bug 1)."""
    flat = _compile_one(
        """
        opcode A = 0x1
        opcode X = 0x2
        opcode D = 0x3
        opcode Y = 0x4
        matcher m {
            produces MB
            build    build_m
            entry    @a
            op A @a <- op X @x -> {op D @d} <- op Y @y
        }
        """,
        A=0x1, X=0x2, D=0x3, Y=0x4,
    )
    grouped = _compile_one(
        """
        opcode A = 0x1
        opcode X = 0x2
        opcode D = 0x3
        opcode Y = 0x4
        matcher m {
            produces MB
            build    build_m
            entry    @a
            op A @a <- ( op X @x -> {op D @d} <- op Y @y )
        }
        """,
        A=0x1, X=0x2, D=0x3, Y=0x4,
    )
    for label, m in (("flat", flat), ("grouped", grouped)):
        assert m.child_matcher is None, label
        assert m.children_matchers is None, label
        x = m.parent_matcher
        assert isinstance(x, ContractMatcher) and x.opcode == 0x2, label
        assert x.children_matchers is not None, label
        assert [cm.opcode for cm in x.children_matchers] == [0x3], label
        y = x.parent_matcher
        assert isinstance(y, ContractMatcher) and y.opcode == 0x4, label
        assert y.parent_matcher is None, label
        assert y.children_matchers is None, label


def test_t04_flat_multi_hop_children_then_parent_edge():
    """Dedust-deposit-final shape: `@a -> {op K} <- op T <- op X -> {op D} <- op Y`
    — the second hop's `<- Y` after its children block is preserved through a
    multi-hop parent chain (regression pin for the flat un-grouped spec)."""
    m = _compile_one(
        """
        opcode A = 0x1
        opcode K = 0x5
        opcode T = 0x6
        opcode X = 0x2
        opcode D = 0x3
        opcode Y = 0x4
        matcher m {
            produces MB
            build    build_m
            entry    @a
            op A @a -> {op K @k} <- op T @t <- op X @x -> {op D @d} <- op Y @y
        }
        """,
        A=0x1, K=0x5, T=0x6, X=0x2, D=0x3, Y=0x4,
    )
    assert [cm.opcode for cm in m.children_matchers] == [0x5]
    t = m.parent_matcher
    assert isinstance(t, ContractMatcher) and t.opcode == 0x6
    x = t.parent_matcher
    assert isinstance(x, ContractMatcher) and x.opcode == 0x2
    assert [cm.opcode for cm in x.children_matchers] == [0x3]
    y = x.parent_matcher
    assert isinstance(y, ContractMatcher) and y.opcode == 0x4
    assert y.parent_matcher is None


def test_t04_upstream_atoms_before_anchor_rejected():
    """`op Up @up -> op Anchor @anchor` with entry @anchor — the upstream `op Up`
    (the anchor's parent) would be silently dropped; the compiler now rejects it
    with C010 rather than misbehaving (T04 Bug 2 / T16 `$rule -> anchor…`)."""
    from indexer.events.mch.diagnostics import CompileError, Severity

    src = """
    opcode Up     = 0x1
    opcode Anchor = 0x2
    matcher m {
        produces MB
        build    build_m
        entry    @anchor
        op Up @up -> op Anchor @anchor
    }
    """
    reg = _registries_with_opcodes(Up=0x1, Anchor=0x2)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    errors = [d for d in exc.value.bag.items if d.severity is Severity.ERROR]
    codes = [d.code for d in errors]
    assert "C010_UPSTREAM_ATOMS_UNSUPPORTED" in codes, codes
    # Span points at the dropped upstream atom (`op Up`), not the anchor.
    diag = next(d for d in errors if d.code == "C010_UPSTREAM_ATOMS_UNSUPPORTED")
    assert "Up" in src[diag.span.start_off:diag.span.end_off], (
        src[diag.span.start_off:diag.span.end_off]
    )


# ==========================================================================
# T33 — inline `where (expr)` sync gate (C013)
# ==========================================================================

def _c013_codes(src: str) -> list[str]:
    from indexer.events.mch.diagnostics import CompileError, Severity
    reg = _registries_with_opcodes(A=0x1, B=0x2)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    return [d.code for d in exc.value.bag.items if d.severity is Severity.ERROR]


def test_where_expr_lookup_is_a_compile_error():
    """Mirror of the IR loader gate: where_expr composes onto sync test_self,
    so a lookup inside it must fail at compile time, not silently never-match."""
    src = """
    matcher m {
        produces MB
        build    build_m
        entry    @a

        op A @a -> op B @b where (lookup jetton_wallet(.owner) == null)
    }
    """
    assert "C013_WHERE_EXPR_NOT_SYNC" in _c013_codes(src)


def test_where_expr_host_fn_call_is_a_compile_error():
    src = """
    matcher m {
        produces MB
        build    build_m
        entry    @a

        op A @a -> op B @b where (my_host_fn(.owner) == 1)
    }
    """
    assert "C013_WHERE_EXPR_NOT_SYNC" in _c013_codes(src)


def test_where_expr_on_group_anchor_branch_is_a_compile_error():
    """Group-anchor branches carry only named `where` predicates in the IR
    anchor record; an inline expr there would be silently dropped — reject."""
    src = """
    matcher m {
        produces MB
        build    build_m
        entry    @a

        (op A @a where (.x == 1) ^ op B @a)
    }
    """
    assert "C013_WHERE_EXPR_NOT_SYNC" in _c013_codes(src)
