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
