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

    matcher behavioral_simple {
        produces FooBlock
        build    build_behavioral_simple
        entry    @t

        op Foo @t
    }
    """
    reg, captured = _registries_with({"Foo": 0x1}, "build_behavioral_simple", "FooBlock")
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

    matcher behavioral_chain {
        produces MB
        build    build_behavioral_chain
        entry    @a

        op A @a -> op B @b
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2}, "build_behavioral_chain", "MB")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    # Tree with A and B in correct relation.
    block = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x2)]))
    assert await m.try_build(block) is not None
    assert len(captured) == 1

    # Tree without B: should NOT match.
    captured.clear()
    bare = build_block_tree(BlockSpec(opcode=0x1))
    result = await m.try_build(bare)
    # try_build may return None or an empty list; both indicate "no match"
    assert not result
    assert len(captured) == 0


@pytest.mark.asyncio
async def test_duplicate_opcode_chain_binds_distinct_blocks(build_block_tree):
    # Phase 1 acceptance test. `op A @first -> op A @second` must compile and
    # the builder must receive `m.first` and `m.second` as the two distinct A
    # blocks from the matched tree.
    src = """
    opcode A = 0x1

    matcher dup_opcode_chain {
        produces MB
        build    build_dup_opcode_chain
        entry    @first

        op A @first -> op A @second
    }
    """
    reg, captured = _registries_with({"A": 0x1}, "build_dup_opcode_chain", "MB")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    # Synthetic tree: A(parent) -> A(child)
    block = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x1)]))
    result = await m.try_build(block)
    assert result is not None
    assert len(captured) == 1
    match = captured[0]
    assert match.first is block, "anchor capture must be the outer A block"
    assert match.second is block.next_blocks[0], "child capture must be the inner A block"
    assert match.first is not match.second


@pytest.mark.asyncio
async def test_captures_stable_across_recompiles(build_block_tree):
    # Phase 1 acceptance test. Compiling the same source twice (with separate
    # Registries — caches are scoped per Registries) must bind captures the
    # same way on the same input tree.
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3

    matcher stability_check {
        produces MB
        build    build_stability_check
        entry    @a

        op A @a -> op B @b -> op C @c
    }
    """
    reg1, captured1 = _registries_with(
        {"A": 0x1, "B": 0x2, "C": 0x3}, "build_stability_check", "MB"
    )
    reg2, captured2 = _registries_with(
        {"A": 0x1, "B": 0x2, "C": 0x3}, "build_stability_check", "MB"
    )
    m1 = compile_text(src, "test.mch", reg1)[0].matcher
    m2 = compile_text(src, "test.mch", reg2)[0].matcher

    block = build_block_tree(
        BlockSpec(opcode=0x1, children=[
            BlockSpec(opcode=0x2, children=[BlockSpec(opcode=0x3)]),
        ])
    )
    await m1.try_build(block)
    await m2.try_build(block)
    assert len(captured1) == 1 and len(captured2) == 1
    a1, a2 = captured1[0], captured2[0]
    assert a1.a is a2.a
    assert a1.b is a2.b
    assert a1.c is a2.c


@pytest.mark.asyncio
async def test_chain_binds_all_captures(build_block_tree):
    # Verify that a linear chain binds every named capture (not just the anchor).
    src = """
    opcode A = 0x1
    opcode B = 0x2

    matcher chain_bind {
        produces MB
        build    build_chain_bind
        entry    @a

        op A @a -> op B @b
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2}, "build_chain_bind", "MB")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    block = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x2)]))
    await m.try_build(block)
    match = captured[0]
    assert match.a is block
    assert match.b is block.next_blocks[0]


@pytest.mark.asyncio
async def test_maybe_wrapped_child_optional(build_block_tree):
    src = """
    opcode A = 0x1
    opcode B = 0x2

    matcher behavioral_maybe {
        produces MB
        build    build_behavioral_maybe
        entry    @a

        op A @a -> maybe op B @b
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2}, "build_behavioral_maybe", "MB")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    # B present.
    with_b = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x2)]))
    assert await m.try_build(with_b) is not None
    assert len(captured) == 1

    # B present: capture is the B block.
    assert captured[0].a is with_b
    assert captured[0].b is with_b.next_blocks[0]

    # B absent — still matches.
    captured.clear()
    without_b = build_block_tree(BlockSpec(opcode=0x1))
    assert await m.try_build(without_b) is not None
    assert len(captured) == 1
    # Optional capture stays None when the branch didn't fire.
    assert captured[0].a is without_b
    assert captured[0].b is None


@pytest.mark.asyncio
async def test_alternative_branch_capture_firing_branch_binds(build_block_tree):
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3

    matcher alt_branches {
        produces MB
        build    build_alt_branches
        entry    @a

        op A @a -> ( op B @b | op C @c )
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2, "C": 0x3}, "build_alt_branches", "MB")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    # B branch fires
    tree_b = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x2)]))
    assert await m.try_build(tree_b) is not None
    assert len(captured) == 1
    assert captured[0].a is tree_b
    assert captured[0].b is tree_b.next_blocks[0]
    assert captured[0].c is None

    # C branch fires
    captured.clear()
    tree_c = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x3)]))
    assert await m.try_build(tree_c) is not None
    assert len(captured) == 1
    assert captured[0].a is tree_c
    assert captured[0].b is None
    assert captured[0].c is tree_c.next_blocks[0]


@pytest.mark.asyncio
async def test_where_predicate_filters_then_binds_capture(build_block_tree):
    src = """
    opcode A = 0x1
    predicate is_special

    matcher where_pred {
        produces MB
        build    build_where_pred
        entry    @a

        op A @a where is_special
    }
    """
    reg, captured = _registries_with({"A": 0x1}, "build_where_pred", "MB")

    is_special_calls: list = []

    @reg.register_predicate("is_special")
    def is_special(block):
        is_special_calls.append(block)
        return getattr(block, "special_flag", False)

    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    # Block without special flag — must not match
    plain = build_block_tree(BlockSpec(opcode=0x1))
    result = await m.try_build(plain)
    assert not result
    assert len(captured) == 0

    # Block with special flag — matches and binds
    special = build_block_tree(BlockSpec(opcode=0x1))
    special.special_flag = True
    result = await m.try_build(special)
    assert result is not None
    assert len(captured) == 1
    assert captured[0].a is special


@pytest.mark.asyncio
async def test_parent_chain_capture_binding(build_block_tree):
    src = """
    opcode A = 0x1
    opcode B = 0x2

    matcher parent_chain {
        produces MB
        build    build_parent_chain
        entry    @a

        op A @a <- op B @b
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2}, "build_parent_chain", "MB")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    # Tree: B(parent) -> A(child). The matcher fires on A.
    tree = build_block_tree(BlockSpec(opcode=0x2, children=[BlockSpec(opcode=0x1)]))
    a_block = tree.next_blocks[0]
    assert a_block.previous_block is tree

    result = await m.try_build(a_block)
    assert result is not None
    assert len(captured) == 1
    assert captured[0].a is a_block
    assert captured[0].b is tree, "parent chain capture must bind to A's previous_block"


@pytest.mark.asyncio
async def test_btype_anchor_capture_binding(build_block_tree):
    src = """
    matcher btype_anchor {
        produces MB
        build    build_btype_anchor
        entry    @j

        btype jetton_transfer @j
    }
    """
    reg, captured = _registries_with({}, "build_btype_anchor", "MB")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    tree = build_block_tree(BlockSpec(btype="jetton_transfer"))
    result = await m.try_build(tree)
    assert result is not None
    assert len(captured) == 1
    assert captured[0].j is tree


@pytest.mark.asyncio
async def test_any_head_capture_binding(build_block_tree):
    src = """
    opcode A = 0x1

    matcher any_head {
        produces MB
        build    build_any_head
        entry    @a

        op A @a -> any @x
    }
    """
    reg, captured = _registries_with({"A": 0x1}, "build_any_head", "MB")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    tree = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(btype="some_random_btype")]))
    result = await m.try_build(tree)
    assert result is not None
    assert len(captured) == 1
    assert captured[0].a is tree
    assert captured[0].x is tree.next_blocks[0]


@pytest.mark.asyncio
async def test_mixed_op_and_btype_chain_capture_binding(build_block_tree):
    src = """
    opcode A = 0x1

    matcher mixed_chain {
        produces MB
        build    build_mixed_chain
        entry    @a

        op A @a -> btype jetton_transfer @j
    }
    """
    reg, captured = _registries_with({"A": 0x1}, "build_mixed_chain", "MB")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    tree = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(btype="jetton_transfer")]))
    result = await m.try_build(tree)
    assert result is not None
    assert len(captured) == 1
    assert captured[0].a is tree
    assert captured[0].j is tree.next_blocks[0]


@pytest.mark.asyncio
async def test_children_block_captures_bind_regardless_of_sibling_order(build_block_tree):
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    opcode X = 0x99

    matcher children_block {
        produces MB
        build    build_children_block
        entry    @a

        op A @a -> {
            op B @b
            op C @c
        }
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2, "C": 0x3, "X": 0x99},
                                     "build_children_block", "MB")
    compiled = compile_text(src, "test.mch", reg)
    m = compiled[0].matcher

    # Order 1: B, C
    captured.clear()
    tree1 = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=0x2), BlockSpec(opcode=0x3),
    ]))
    await m.try_build(tree1)
    assert len(captured) == 1
    assert captured[0].b is tree1.next_blocks[0]
    assert captured[0].c is tree1.next_blocks[1]

    # Order 2: C, B (siblings reversed in tree)
    captured.clear()
    tree2 = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=0x3), BlockSpec(opcode=0x2),
    ]))
    await m.try_build(tree2)
    assert len(captured) == 1, "children block should match regardless of sibling order"
    assert captured[0].b is tree2.next_blocks[1], "b capture must be the B block"
    assert captured[0].c is tree2.next_blocks[0], "c capture must be the C block"

    # Order 3: X (unrelated), B, C — unrelated sibling should not break the match
    captured.clear()
    tree3 = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=0x99), BlockSpec(opcode=0x2), BlockSpec(opcode=0x3),
    ]))
    await m.try_build(tree3)
    assert len(captured) == 1
    assert captured[0].b is tree3.next_blocks[1]
    assert captured[0].c is tree3.next_blocks[2]


@pytest.mark.asyncio
async def test_build_block_returns_full_consumed_set(build_block_tree):
    """Phase 2: synthesized build_block must call produced.merge_blocks([block]+other_blocks)
    for absorption and return [produced] only (mirroring hand-written matchers like
    elections.py:46-47)."""
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3

    matcher full_consumed {
        produces MB
        build    build_full_consumed
        entry    @a

        op A @a -> op B @b -> op C @c
    }
    """
    reg = Registries()
    reg.opcodes.update({"A": 0x1, "B": 0x2, "C": 0x3})

    from indexer.events.blocks.core import Block

    class _MarkerBlock(Block):
        def __init__(self):
            super().__init__("marker", [], None)
            self.merge_calls: list = []
        def merge_blocks(self, blocks):  # type: ignore[override]
            # Capture the call without executing the real merge — fake blocks lack
            # event_nodes and would crash calculate_min_max_lt.
            self.merge_calls.append(list(blocks))

    reg.block_types["MB"] = _MarkerBlock

    @reg.register_builder("build_full_consumed")
    async def _build(m):
        return _MarkerBlock()

    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    a = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=0x2, children=[BlockSpec(opcode=0x3)]),
    ]))
    b = a.next_blocks[0]
    c = b.next_blocks[0]

    result = await matcher.try_build(a)
    assert result is not None
    # Return list shape: [produced] only — mirrors hand-written matchers
    # (e.g. elections.py:46-47). Absorption is via merge_blocks side effect.
    # Excess/bounce auto-append doesn't fire here because tree has no excess/bounce blocks.
    assert len(result) == 1, f"build_block must return single-element [produced], got {len(result)} elements: {result!r}"
    assert isinstance(result[0], _MarkerBlock), "first (only) element must be produced block"
    produced = result[0]
    # Absorption: merge_blocks called once with [a, b, c].
    assert len(produced.merge_calls) == 1, f"expected one merge_blocks call, got {len(produced.merge_calls)}"
    assert produced.merge_calls[0] == [a, b, c], (
        f"merge_blocks called with wrong list: {produced.merge_calls[0]!r}"
    )


@pytest.mark.asyncio
async def test_build_block_empty_when_builder_returns_none(build_block_tree):
    """Phase 2: when builder returns None (match rejected), no absorption happens
    and no excess/bounce leak occurs."""
    from indexer.events.blocks.messages import ExcessMessage

    src = """
    opcode A = 0x1

    matcher rejecting {
        produces MB
        build    build_rejecting
        entry    @a

        op A @a
    }
    """
    reg = Registries()
    reg.opcodes.update({"A": 0x1})
    reg.block_types["MB"] = type("MB", (), {})

    @reg.register_builder("build_rejecting")
    async def _build(m):
        return None  # always rejects

    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    # Variant A: trivial tree, no excess children. Must short-circuit cleanly.
    bare = build_block_tree(BlockSpec(opcode=0x1))
    result = await matcher.try_build(bare)
    assert not result, f"rejected match (no excess) must produce empty result, got {result!r}"
    assert bare.parent is None, "rejected match must not set block.parent"

    # Variant B: tree where anchor has an excess child block. The engine's
    # auto-append step (basic_matchers.py:53-57) appends excess blocks to whatever
    # build_block returns. If build_block returns [], auto-append turns it into
    # [excess_block] — a leak. Verify this doesn't happen.
    with_excess = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=ExcessMessage.opcode),
    ]))
    result2 = await matcher.try_build(with_excess)
    assert not result2, (
        f"rejected match must produce empty result even with excess children, got {result2!r}. "
        f"If non-empty, the auto-append leaked excess blocks into a rejected match."
    )
    assert with_excess.parent is None
    assert with_excess.next_blocks[0].parent is None, "excess block must not be absorbed by rejected match"


# ==========================================================================
# Phase 4 — multi-hop parent chains
# ==========================================================================

@pytest.mark.asyncio
async def test_two_hop_parent_chain_anchor_head(build_block_tree):
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
    reg, captured = _registries_with({"A": 0x1, "B": 0x2, "C": 0x3}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    # Tree: C(child=B(child=A))
    tree = build_block_tree(BlockSpec(opcode=0x3, children=[
        BlockSpec(opcode=0x2, children=[BlockSpec(opcode=0x1)]),
    ]))
    b_block = tree.next_blocks[0]
    a_block = b_block.next_blocks[0]
    assert a_block.previous_block is b_block
    assert b_block.previous_block is tree

    result = await matcher.try_build(a_block)
    assert result is not None
    assert len(captured) == 1
    assert captured[0].a is a_block
    assert captured[0].b is b_block
    assert captured[0].c is tree


@pytest.mark.asyncio
async def test_three_hop_parent_chain_anchor_head(build_block_tree):
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    opcode D = 0x4

    matcher m {
        produces MB
        build    build_m
        entry    @a

        op A @a <- op B @b <- op C @c <- op D @d
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2, "C": 0x3, "D": 0x4}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    # Tree: D(child=C(child=B(child=A)))
    tree = build_block_tree(BlockSpec(opcode=0x4, children=[
        BlockSpec(opcode=0x3, children=[
            BlockSpec(opcode=0x2, children=[BlockSpec(opcode=0x1)]),
        ]),
    ]))
    c_block = tree.next_blocks[0]
    b_block = c_block.next_blocks[0]
    a_block = b_block.next_blocks[0]

    result = await matcher.try_build(a_block)
    assert result is not None
    assert captured[0].a is a_block
    assert captured[0].b is b_block
    assert captured[0].c is c_block
    assert captured[0].d is tree


@pytest.mark.asyncio
async def test_two_hop_parent_chain_partial_match_rejected(build_block_tree):
    """Root parent has wrong opcode: match fails."""
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    opcode X = 0x99

    matcher m {
        produces MB
        build    build_m
        entry    @a

        op A @a <- op B @b <- op C @c
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2, "C": 0x3, "X": 0x99}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    # Tree: X(child=B(child=A)) — root is X, not C
    tree = build_block_tree(BlockSpec(opcode=0x99, children=[
        BlockSpec(opcode=0x2, children=[BlockSpec(opcode=0x1)]),
    ]))
    a_block = tree.next_blocks[0].next_blocks[0]

    result = await matcher.try_build(a_block)
    assert not result
    assert len(captured) == 0


@pytest.mark.asyncio
async def test_parent_chain_in_tail_position_child_edge_anchor(build_block_tree):
    """Anchor in tail with CHILD incoming edge, followed by PARENT chain."""
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
    reg, captured = _registries_with({"X": 0x9, "A": 0x1, "B": 0x2, "C": 0x3}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    # Tree (block-tree relations):
    # The pattern says A has parent B has parent C. Build tree as: C(child=B(child=A)),
    # test try_build on A.
    tree = build_block_tree(BlockSpec(opcode=0x3, children=[
        BlockSpec(opcode=0x2, children=[BlockSpec(opcode=0x1)]),
    ]))
    a_block = tree.next_blocks[0].next_blocks[0]

    result = await matcher.try_build(a_block)
    # X is NOT validated as A's child in this DSL position; the pattern says
    # "A has parent B has parent C". Tree satisfies that. Match should succeed.
    assert result is not None
    assert captured[0].a is a_block
    assert captured[0].b is a_block.previous_block
    assert captured[0].c is tree


@pytest.mark.asyncio
async def test_parent_chain_inside_alternative_branch(build_block_tree):
    """Alternative with one branch carrying a parent chain."""
    src = """
    opcode Anchor = 0x7
    opcode A = 0x1
    opcode B = 0x2
    opcode X = 0x9

    matcher m {
        produces MB
        build    build_m
        entry    @anchor

        op Anchor @anchor -> ( op A @a <- op B @b | op X @x )
    }
    """
    reg, captured = _registries_with({"Anchor": 0x7, "A": 0x1, "B": 0x2, "X": 0x9}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    # Tree: anchor.child = X — branch 2 fires.
    tree = build_block_tree(BlockSpec(opcode=0x7, children=[BlockSpec(opcode=0x9)]))
    result = await matcher.try_build(tree)
    assert result is not None
    assert captured[0].x is tree.next_blocks[0]
    assert captured[0].a is None
    assert captured[0].b is None


@pytest.mark.asyncio
async def test_parent_chain_inside_maybe_subtree(build_block_tree):
    """Parent chain whose link is wrapped in maybe — optional link."""
    src = """
    opcode A = 0x1
    opcode B = 0x2

    matcher m {
        produces MB
        build    build_m
        entry    @a

        op A @a <- maybe op B @b
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    # Tree with B as A's parent — capture binds.
    tree_with = build_block_tree(BlockSpec(opcode=0x2, children=[BlockSpec(opcode=0x1)]))
    a_with = tree_with.next_blocks[0]
    result1 = await matcher.try_build(a_with)
    assert result1 is not None
    assert captured[0].a is a_with
    assert captured[0].b is tree_with
    captured.clear()

    # Tree with no parent (A standalone) — match still succeeds because B is optional.
    tree_alone = build_block_tree(BlockSpec(opcode=0x1))
    result2 = await matcher.try_build(tree_alone)
    assert result2 is not None
    assert captured[0].a is tree_alone
    assert captured[0].b is None


@pytest.mark.asyncio
async def test_parent_chain_mixed_edge_after_chain(build_block_tree):
    """`op A @a -> op B -> op D` validates the simpler chain compiles and matches normally.

    The original goal `op A @a -> op B <- op C @c -> op D` is a pre-existing limitation
    of the lowering (the `<- op C` segment is dropped). Phase 4 does NOT improve this."""
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    opcode D = 0x4

    matcher m {
        produces MB
        build    build_m
        entry    @a

        op A @a -> op B -> op D
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2, "D": 0x4}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    tree = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=0x2, children=[BlockSpec(opcode=0x4)]),
    ]))
    result = await matcher.try_build(tree)
    assert result is not None
    assert captured[0].a is tree


@pytest.mark.asyncio
async def test_anchor_in_tail_parent_edge_legacy_behavior(build_block_tree):
    """Variant B.1 — anchor-in-tail with incoming PARENT edge.

    Spec §2.6: preserve existing single-hop behavior in v0; we assert what the
    implementation produces (not a hand-derived ground truth). This test exists
    to lock current behavior so future refactors don't silently change it.
    """
    src = """
    opcode X = 0x9
    opcode A = 0x1

    matcher m {
        produces MB
        build    build_m
        entry    @a

        op X <- op A @a
    }
    """
    reg, captured = _registries_with({"X": 0x9, "A": 0x1}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    # Per existing single-hop behavior: A.parent_matcher = lower(X), meaning
    # the matcher expects A.previous_block to satisfy X. So we test against a tree
    # where A.previous_block = X.
    tree = build_block_tree(BlockSpec(opcode=0x9, children=[BlockSpec(opcode=0x1)]))
    a_block = tree.next_blocks[0]
    result = await matcher.try_build(a_block)
    assert result is not None
    assert captured[0].a is a_block


# ==========================================================================
# Phase 3 — list bindings under recursion
# ==========================================================================

@pytest.mark.asyncio
async def test_recursive_rule_three_iterations_list_capture(build_block_tree):
    """3-hop recursion: `m.a == [A1, A2, A3]`."""
    src = """
    opcode A = 0x1

    rule step = op A @a -> maybe $step

    matcher m {
        produces MB
        build    build_m
        entry    @anchor

        op A @anchor -> $step
    }
    """
    reg, captured = _registries_with({"A": 0x1}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    # Trace: anchor(opcode=A, child=A1(child=A2(child=A3)))
    tree = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=0x1, children=[
            BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x1)]),
        ]),
    ]))
    result = await matcher.try_build(tree)
    assert result is not None
    assert captured[0].anchor is tree
    # `step` recursion captures the A's under anchor — should be 3 hops.
    assert isinstance(captured[0].a, list), f"m.a expected list, got {type(captured[0].a)!r}"
    assert len(captured[0].a) == 3, f"expected 3 iterations, got {len(captured[0].a)}"


@pytest.mark.asyncio
async def test_recursive_rule_parallel_captures_aligned(build_block_tree):
    """Parallel captures align per iteration."""
    src = """
    opcode A = 0x1
    opcode B = 0x2

    rule step = op A @a -> op B @b -> maybe $step

    matcher m {
        produces MB
        build    build_m
        entry    @anchor

        op A @anchor -> $step
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    # Trace: anchor → A1 → B1 → A2 → B2 → A3 → B3
    def chain(opcs):
        if not opcs:
            return []
        return [BlockSpec(opcode=opcs[0], children=chain(opcs[1:]))]

    tree = build_block_tree(BlockSpec(opcode=0x1, children=chain([0x1, 0x2, 0x1, 0x2, 0x1, 0x2])))
    result = await matcher.try_build(tree)
    assert result is not None
    a_list = captured[0].a
    b_list = captured[0].b
    assert len(a_list) == 3 and len(b_list) == 3
    for i in range(3):
        assert a_list[i].opcode == 0x1
        assert b_list[i].opcode == 0x2


@pytest.mark.asyncio
async def test_recursive_rule_zero_iterations_empty_list(build_block_tree):
    """No iterations: capture is `[]`, not None."""
    src = """
    opcode A = 0x1
    opcode B = 0x2

    rule step = op B @b -> maybe $step

    matcher m {
        produces MB
        build    build_m
        entry    @anchor

        op A @anchor -> maybe $step
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    # Trace: anchor with no B children → recursion fires 0 times.
    tree = build_block_tree(BlockSpec(opcode=0x1))
    result = await matcher.try_build(tree)
    assert result is not None
    assert captured[0].anchor is tree
    assert captured[0].b == [], f"expected empty list, got {captured[0].b!r}"


@pytest.mark.asyncio
async def test_recursive_rule_one_iteration_single_element_list(build_block_tree):
    """Single-hop trace: list has one element."""
    src = """
    opcode A = 0x1

    rule step = op A @a -> maybe $step

    matcher m {
        produces MB
        build    build_m
        entry    @anchor

        op A @anchor -> $step
    }
    """
    reg, captured = _registries_with({"A": 0x1}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    # Trace: anchor → A1 (single hop)
    tree = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x1)]))
    result = await matcher.try_build(tree)
    assert result is not None
    assert len(captured[0].a) == 1
    assert captured[0].a[0] is tree.next_blocks[0]


@pytest.mark.asyncio
async def test_recursive_rule_termination_no_pure_exit(build_block_tree):
    """Regression for the v0 'always-one-iteration' bug.

    Step branches all carry `maybe $self` — no pure exit. Phase 3 strips the
    trailing self-refs and lets the engine iterate via natural step failure."""
    src = """
    opcode A = 0x1

    rule step = (
          op A @a -> maybe $step
        | maybe $step
    )

    matcher m {
        produces MB
        build    build_m
        entry    @anchor

        op A @anchor -> $step
    }
    """
    reg, captured = _registries_with({"A": 0x1}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    # Trace: anchor → A1 → A2 → A3 (3 hops); recursion must iterate 3 times.
    tree = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=0x1, children=[
            BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x1)]),
        ]),
    ]))
    result = await matcher.try_build(tree)
    assert result is not None
    assert len(captured[0].a) == 3, (
        f"expected 3 iterations (regression for the v0 one-iteration bug), "
        f"got {len(captured[0].a)}"
    )


@pytest.mark.asyncio
async def test_recursive_rule_with_maybe_inside_step(build_block_tree):
    """Per-iter optional captures inside recursion."""
    src = """
    opcode A = 0x1
    opcode B = 0x2

    rule step = op A @a -> maybe op B @b -> maybe $step

    matcher m {
        produces MB
        build    build_m
        entry    @anchor

        op A @anchor -> $step
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    # Trace: anchor → A1 → A2 → B2 → A3 (B present only on iter 2)
    tree = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=0x1, children=[  # A1
            BlockSpec(opcode=0x1, children=[  # A2
                BlockSpec(opcode=0x2, children=[  # B2
                    BlockSpec(opcode=0x1)  # A3
                ]),
            ]),
        ]),
    ]))
    result = await matcher.try_build(tree)
    assert result is not None
    a_list = captured[0].a
    b_list = captured[0].b
    assert len(a_list) == 3
    assert len(b_list) == 3
    assert b_list[0] is None
    assert b_list[1] is not None and b_list[1].opcode == 0x2
    # b_list[2] semantics depends on whether the engine finds B after A3 — likely None.
    # Don't over-assert.


@pytest.mark.asyncio
async def test_recursive_rule_outside_captures_remain_scalar(build_block_tree):
    """Outside-recursion captures stay scalar."""
    src = """
    opcode A = 0x1
    opcode B = 0x2

    rule step = op B @b -> maybe $step

    matcher m {
        produces MB
        build    build_m
        entry    @anchor

        op A @anchor -> $step
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    tree = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x2)]))
    result = await matcher.try_build(tree)
    assert result is not None
    # @anchor is OUTSIDE the recursive rule → scalar
    assert captured[0].anchor is tree, "outside-recursion capture must be scalar"
    # @b is INSIDE the recursive rule → list
    assert isinstance(captured[0].b, list)
    assert len(captured[0].b) == 1


@pytest.mark.asyncio
async def test_capture_after_recursion_rejected():
    """C007: captures after a recursive rule reference in the same sequence."""
    from indexer.events.mch.diagnostics import CompileError, Severity

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
    reg = Registries()
    reg.opcodes.update({"A": 0x1, "X": 0x9})
    reg.block_types["MB"] = type("MB", (), {})
    reg.register_builder("build_m")(lambda m: None)
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    codes = [d.code for d in exc.value.bag.items if d.severity is Severity.ERROR]
    assert "C007_CAPTURE_AFTER_RECURSION" in codes, codes


@pytest.mark.asyncio
async def test_recursive_rule_or_branches_different_captures(build_block_tree):
    """OrMatcher inside step: each iter binds the firing branch's capture."""
    src = """
    opcode A = 0x1
    opcode B = 0x2

    rule step = ( op A @a | op B @b ) -> maybe $step

    matcher m {
        produces MB
        build    build_m
        entry    @anchor

        op A @anchor -> $step
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    # Trace: anchor → A1 → B2 → A3 (alternating)
    tree = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=0x1, children=[  # A1 (iter 1 fires A-branch)
            BlockSpec(opcode=0x2, children=[  # B2 (iter 2 fires B-branch)
                BlockSpec(opcode=0x1)  # A3 (iter 3 fires A-branch)
            ]),
        ]),
    ]))
    result = await matcher.try_build(tree)
    assert result is not None
    a_list = captured[0].a
    b_list = captured[0].b
    assert len(a_list) == 3 and len(b_list) == 3
    assert a_list[0] is not None and a_list[0].opcode == 0x1  # iter 1: A
    assert b_list[0] is None
    assert a_list[1] is None
    assert b_list[1] is not None and b_list[1].opcode == 0x2  # iter 2: B
    assert a_list[2] is not None and a_list[2].opcode == 0x1  # iter 3: A
    assert b_list[2] is None


# ==========================================================================
# Phase 7 — XOR separator behavioral tests
# ==========================================================================

@pytest.mark.asyncio
async def test_exclusive_or_behavioral_double_match_rejects(build_block_tree):
    """ExclusiveOrMatcher.test_self returns False when both branches match one block."""
    from indexer.events.blocks.basic_matchers import ContractMatcher, ExclusiveOrMatcher

    # Two branches with same opcode — both test_self return True on the same block.
    xor = ExclusiveOrMatcher([ContractMatcher(0x1), ContractMatcher(0x1)])
    block = build_block_tree(BlockSpec(opcode=0x1))
    # sum == 2 → test_self must return False (exclusive-or semantics).
    assert xor.test_self(block) is False


@pytest.mark.asyncio
async def test_caret_inside_recursive_rule(build_block_tree):
    """rule r = (op A @a ^ maybe $r) lowers cleanly (Phase 3 regression guard)."""
    from indexer.events.blocks.basic_matchers import ExclusiveOrMatcher
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
    reg, captured = _registries_with({"A": 0x1}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    assert compiled, "compilation must succeed"


@pytest.mark.asyncio
async def test_exclusive_or_fires_correct_branch(build_block_tree):
    """ExclusiveOrMatcher child routes to correct branch when exactly one matches."""
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3

    matcher m {
        produces MB
        build    build_m
        entry    @a

        op A @a -> (op B @b ^ op C @c)
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2, "C": 0x3}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    # B child fires → @b bound, @c is None.
    tree = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x2)]))
    result = await matcher.try_build(tree)
    assert result is not None
    assert captured[0].a is tree
    assert captured[0].b is tree.next_blocks[0]
    assert captured[0].c is None


# ==========================================================================
# Phase 5 — ChildrenBlock attach-to-left
# ==========================================================================

@pytest.mark.asyncio
async def test_children_block_after_anchor_unchanged(build_block_tree):
    """Regression: op A @a -> {op B; op C} (anchor-direct fast path)."""
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3

    matcher m {
        produces MB
        build    build_m
        entry    @a
        op A @a -> {op B @b; op C @c}
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2, "C": 0x3}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher
    tree = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=0x2),
        BlockSpec(opcode=0x3),
    ]))
    result = await matcher.try_build(tree)
    assert result is not None
    assert captured[0].a is tree
    assert captured[0].b is not None
    assert captured[0].c is not None


@pytest.mark.asyncio
async def test_children_block_mid_chain_binds_to_left(build_block_tree):
    """op A -> op X -> {op B; op C}: X gets children_matchers=[B, C]."""
    src = """
    opcode A = 0x1
    opcode X = 0x9
    opcode B = 0x2
    opcode C = 0x3

    matcher m {
        produces MB
        build    build_m
        entry    @a
        op A @a -> op X @x -> {op B @b; op C @c}
    }
    """
    reg, captured = _registries_with({"A": 0x1, "X": 0x9, "B": 0x2, "C": 0x3}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher
    tree = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=0x9, children=[
            BlockSpec(opcode=0x2),
            BlockSpec(opcode=0x3),
        ]),
    ]))
    result = await matcher.try_build(tree)
    assert result is not None
    assert captured[0].a is tree
    assert captured[0].x is tree.next_blocks[0]
    assert captured[0].b is not None
    assert captured[0].c is not None


@pytest.mark.asyncio
async def test_children_block_mid_chain_continues_after(build_block_tree):
    """op A -> {op B; op C} -> op D: A has children_matchers=[B, C] AND child_matcher=D."""
    src = """
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
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2, "C": 0x3, "D": 0x4}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher
    tree = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=0x2),
        BlockSpec(opcode=0x3),
        BlockSpec(opcode=0x4),
    ]))
    result = await matcher.try_build(tree)
    assert result is not None
    assert captured[0].b is not None
    assert captured[0].c is not None
    assert captured[0].d is not None


def test_children_block_inside_alternative_branch_rejected():
    """(op A | {op B; op C}) → C008 compile error."""
    from indexer.events.mch.diagnostics import CompileError
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
    reg, _ = _registries_with({"A": 0x1, "B": 0x2, "C": 0x3}, "build_m", "MB")
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    codes = [d.code for d in exc.value.bag.items]
    assert "C008_CHILDREN_BLOCK_BARE" in codes, codes


def test_children_block_inside_bare_maybe_rejected():
    """maybe {op B; op C} as standalone → C008 compile error."""
    from indexer.events.mch.diagnostics import CompileError
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
    reg, _ = _registries_with({"A": 0x1, "B": 0x2, "C": 0x3}, "build_m", "MB")
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    codes = [d.code for d in exc.value.bag.items]
    assert "C008_CHILDREN_BLOCK_BARE" in codes, codes


@pytest.mark.asyncio
async def test_maybe_wrapped_children_block_in_chain_attaches_optional(build_block_tree):
    """op A -> maybe {op B; op C}: items attach as optional children of A."""
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3

    matcher m {
        produces MB
        build    build_m
        entry    @a
        op A @a -> maybe {op B @b; op C @c}
    }
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2, "C": 0x3}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    tree_empty = build_block_tree(BlockSpec(opcode=0x1))
    result = await matcher.try_build(tree_empty)
    assert result is not None

    tree_full = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=0x2),
        BlockSpec(opcode=0x3),
    ]))
    result = await matcher.try_build(tree_full)
    assert result is not None


def test_children_block_as_sequence_head_rejected():
    """{op B; op C} -> op A as head of sequence → C008."""
    from indexer.events.mch.diagnostics import CompileError
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
    reg, _ = _registries_with({"A": 0x1, "B": 0x2, "C": 0x3}, "build_m", "MB")
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    codes = [d.code for d in exc.value.bag.items]
    assert "C008_CHILDREN_BLOCK_BARE" in codes, codes


def test_children_block_rule_body_root_rejected():
    """rule r = {op A; op B} referenced from chain → C008."""
    from indexer.events.mch.diagnostics import CompileError
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
    reg, _ = _registries_with({"A": 0x1, "B": 0x2, "X": 0x9}, "build_m", "MB")
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    codes = [d.code for d in exc.value.bag.items]
    assert "C008_CHILDREN_BLOCK_BARE" in codes, codes


@pytest.mark.asyncio
async def test_children_block_after_parent_edge(build_block_tree):
    """op A @a <- op B @b -> {op C; op D}: B has children_matchers=[C, D]."""
    src = """
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
    """
    reg, captured = _registries_with({"A": 0x1, "B": 0x2, "C": 0x3, "D": 0x4}, "build_m", "MB")
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher
    tree = build_block_tree(BlockSpec(opcode=0x2, children=[
        BlockSpec(opcode=0x1),
        BlockSpec(opcode=0x3),
        BlockSpec(opcode=0x4),
    ]))
    a_block = next(b for b in tree.next_blocks if b.opcode == 0x1)
    result = await matcher.try_build(a_block)
    assert result is not None
    assert captured[0].b is tree
    assert captured[0].c is not None
    assert captured[0].d is not None


@pytest.mark.asyncio
async def test_two_back_to_back_children_blocks(build_block_tree):
    """op A -> {op B; op C} -> {op D; op E}: A has children_matchers=[B, C, D, E]."""
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    opcode D = 0x4
    opcode E = 0x5

    matcher m {
        produces MB
        build    build_m
        entry    @a
        op A @a -> {op B @b; op C @c} -> {op D @d; op E @e}
    }
    """
    reg, captured = _registries_with(
        {"A": 0x1, "B": 0x2, "C": 0x3, "D": 0x4, "E": 0x5}, "build_m", "MB"
    )
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher
    tree = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=0x2),
        BlockSpec(opcode=0x3),
        BlockSpec(opcode=0x4),
        BlockSpec(opcode=0x5),
    ]))
    result = await matcher.try_build(tree)
    assert result is not None
    assert captured[0].b is not None
    assert captured[0].c is not None
    assert captured[0].d is not None
    assert captured[0].e is not None


@pytest.mark.asyncio
async def test_anchor_adjacent_plus_mid_chain_children_blocks(build_block_tree):
    """op A -> {B;C} -> op X -> {D;E}: A gets [B,C] children; X gets [D,E] children."""
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    opcode X = 0x9
    opcode D = 0x4
    opcode E = 0x5

    matcher m {
        produces MB
        build    build_m
        entry    @a
        op A @a -> {op B @b; op C @c} -> op X @x -> {op D @d; op E @e}
    }
    """
    reg, captured = _registries_with(
        {"A": 0x1, "B": 0x2, "C": 0x3, "X": 0x9, "D": 0x4, "E": 0x5}, "build_m", "MB"
    )
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher
    tree = build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=0x2),
        BlockSpec(opcode=0x3),
        BlockSpec(opcode=0x9, children=[
            BlockSpec(opcode=0x4),
            BlockSpec(opcode=0x5),
        ]),
    ]))
    result = await matcher.try_build(tree)
    assert result is not None
    assert captured[0].b is not None
    assert captured[0].c is not None
    assert captured[0].x is tree.next_blocks[2]
    assert captured[0].d is not None
    assert captured[0].e is not None


@pytest.mark.asyncio
async def test_same_name_capture_across_xor_binds_winning_branch(build_block_tree):
    """Spec §6.2(a) / T02: `op R @r -> (op A @x ^ op B @x)` — @x binds whichever
    branch actually matched; the losing branch leaves nothing behind."""
    src = """
    opcode R = 0x1
    opcode A = 0x2
    opcode B = 0x3

    matcher xor_same_capture {
        produces MB
        build    build_xor_same_capture
        entry    @r

        op R @r -> (op A @x ^ op B @x)
    }
    """
    reg, captured = _registries_with(
        {"R": 0x1, "A": 0x2, "B": 0x3}, "build_xor_same_capture", "MB"
    )
    compiled = compile_text(src, "test.mch", reg)
    matcher = compiled[0].matcher

    # Branch A wins.
    tree_a = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x2)]))
    assert await matcher.try_build(tree_a) is not None
    assert captured[0].r is tree_a
    assert captured[0].x is tree_a.next_blocks[0]

    # Branch B wins.
    captured.clear()
    tree_b = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x3)]))
    assert await matcher.try_build(tree_b) is not None
    assert captured[0].r is tree_b
    assert captured[0].x is tree_b.next_blocks[0]
