"""T18 — §7.2 list-capture conformance suite.

Pins the alignment rule for captures under recursion as implemented (and as
spec §7.2 states after the T18 amendment):

- step-branch captures: exactly one element per unrolling, parallel-aligned,
  `None` for unrollings where the capture's branch/`maybe` did not participate
  (gap alignment — decision T18-b);
- exit-branch captures: bind at most once, on termination via the pure exit;
  `[]` when the recursion never fired or ended by frontier exhaustion
  (decision T18-a);
- captures positioned after the recursion in traversal order (later
  children-set items, parent chain) bind normally on zero-, one- and
  multi-hop matches (decision T18-c);
- `maybe $rule` absent ⇒ `[]`, elements in unrolling (trace) order, terminal
  access via `xs[-1]`;
- `Match._mch_consumed` is de-duplicated (first occurrence wins) — the raw
  engine list reports a recursion's entry block twice (T16 finding).

All trees are synthetic (conftest BlockSpec), following the behavioral-test
fixture style.
"""
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


_OPCODES = {
    "Anchor": 0x10,
    "Step": 0x11,
    "Note": 0x12,
    "Exit": 0x20,
    "ExitChild": 0x21,
    "Direct": 0x30,
    "Tail": 0x40,
    "Parent": 0x50,
}

# Dedust-shaped pattern: recursion as one branch of a children-set alternative,
# with a later children-set item and a parent edge — the two post-recursion
# capture positions from the T15 gap (c).
_SRC = """
opcode Anchor    = 0x10
opcode Step      = 0x11
opcode Note      = 0x12
opcode Exit      = 0x20
opcode ExitChild = 0x21
opcode Direct    = 0x30
opcode Tail      = 0x40
opcode Parent    = 0x50

rule chain = (
    op Step @s -> { maybe op Note @note } -> maybe $chain
  ^ op Exit @exit_a -> op ExitChild @exit_b
)

matcher m {
    produces MB
    build    build_m
    entry    @anchor

    op Anchor @anchor
      -> { ( op Direct @direct | $chain ) ; maybe op Tail @tail }
      <- op Parent @parent
}
"""


def _compile():
    reg, captured = _registries_with(_OPCODES, "build_m", "MB")
    matcher = compile_text(_SRC, "test.mch", reg)[0].matcher
    return matcher, captured


def _three_hop_tree(build_block_tree):
    """Parent(Anchor(Step1(Step2(Note2, Step3(Exit(ExitChild)))), Tail))."""
    parent = build_block_tree(BlockSpec(opcode=0x50, children=[
        BlockSpec(opcode=0x10, children=[
            BlockSpec(opcode=0x11, children=[                      # Step1
                BlockSpec(opcode=0x11, children=[                  # Step2
                    BlockSpec(opcode=0x12),                        # Note2
                    BlockSpec(opcode=0x11, children=[              # Step3
                        BlockSpec(opcode=0x20, children=[          # Exit
                            BlockSpec(opcode=0x21),                # ExitChild
                        ]),
                    ]),
                ]),
            ]),
            BlockSpec(opcode=0x40),                                # Tail
        ]),
    ]))
    anchor = parent.next_blocks[0]
    step1 = anchor.next_blocks[0]
    tail = anchor.next_blocks[1]
    step2 = step1.next_blocks[0]
    note2 = step2.next_blocks[0]
    step3 = step2.next_blocks[1]
    exit_ = step3.next_blocks[0]
    exit_child = exit_.next_blocks[0]
    return parent, anchor, step1, step2, step3, note2, tail, exit_, exit_child


@pytest.mark.asyncio
async def test_three_hop_step_captures_parallel_aligned_trace_order(build_block_tree):
    """Unconditional step-path capture: one element per unrolling, in unrolling
    (trace) order; `maybe`-in-step capture gap-aligned with explicit Nones."""
    matcher, captured = _compile()
    (parent, anchor, step1, step2, step3, note2,
     tail, exit_, exit_child) = _three_hop_tree(build_block_tree)

    assert await matcher.try_build(anchor) is not None
    m = captured[0]
    assert m.anchor is anchor
    # Step-path capture: exactly one element per unrolling, ascending trace order.
    assert m.s == [step1, step2, step3]
    # Gap alignment (T18-b): the `maybe`-wrapped capture stays parallel to the
    # step list; unrollings without the optional child contribute None.
    assert m.note == [None, note2, None]
    assert len(m.note) == len(m.s)
    # Terminal access pattern.
    assert m.s[-1] is step3


@pytest.mark.asyncio
async def test_exit_branch_captures_bind_once_no_padding(build_block_tree):
    """Exit-branch captures (T18-a): bound exactly once on termination via the
    pure exit — single-element lists, no per-unrolling None padding."""
    matcher, captured = _compile()
    (parent, anchor, step1, step2, step3, note2,
     tail, exit_, exit_child) = _three_hop_tree(build_block_tree)

    assert await matcher.try_build(anchor) is not None
    m = captured[0]
    assert m.exit_a == [exit_]
    assert m.exit_b == [exit_child]


@pytest.mark.asyncio
async def test_post_recursion_captures_bind_on_multi_hop(build_block_tree):
    """T18-c: captures after the recursion in traversal order (later children-set
    item, parent chain) bind on a multi-hop match; the losing alternative
    branch stays None."""
    matcher, captured = _compile()
    (parent, anchor, step1, step2, step3, note2,
     tail, exit_, exit_child) = _three_hop_tree(build_block_tree)

    assert await matcher.try_build(anchor) is not None
    m = captured[0]
    assert m.tail is tail, "children-set item after the recursion must bind"
    assert m.parent is parent, "parent capture after the recursion must bind"
    assert m.direct is None, "losing alternative branch must stay None"


@pytest.mark.asyncio
async def test_single_hop_non_recursive_alternative_binds_post_captures(build_block_tree):
    """T18-c single-hop case: the non-recursive alternative wins; post-alternative
    captures bind and every recursion list stays []."""
    matcher, captured = _compile()
    parent = build_block_tree(BlockSpec(opcode=0x50, children=[
        BlockSpec(opcode=0x10, children=[
            BlockSpec(opcode=0x30),      # Direct
            BlockSpec(opcode=0x40),      # Tail
        ]),
    ]))
    anchor = parent.next_blocks[0]
    direct = anchor.next_blocks[0]
    tail = anchor.next_blocks[1]

    assert await matcher.try_build(anchor) is not None
    m = captured[0]
    assert m.direct is direct
    assert m.tail is tail
    assert m.parent is parent
    assert m.s == []
    assert m.note == []
    assert m.exit_a == []
    assert m.exit_b == []


@pytest.mark.asyncio
async def test_one_hop_recursion_shape(build_block_tree):
    """One unrolling then the exit: single-element step lists, exit bound."""
    matcher, captured = _compile()
    parent = build_block_tree(BlockSpec(opcode=0x50, children=[
        BlockSpec(opcode=0x10, children=[
            BlockSpec(opcode=0x11, children=[            # Step1
                BlockSpec(opcode=0x20, children=[        # Exit
                    BlockSpec(opcode=0x21),              # ExitChild
                ]),
            ]),
            BlockSpec(opcode=0x40),                      # Tail
        ]),
    ]))
    anchor = parent.next_blocks[0]
    step1 = anchor.next_blocks[0]
    tail = anchor.next_blocks[1]
    exit_ = step1.next_blocks[0]

    assert await matcher.try_build(anchor) is not None
    m = captured[0]
    assert m.s == [step1]
    assert m.note == [None]
    assert m.exit_a == [exit_]
    assert m.exit_b == [exit_.next_blocks[0]]
    assert m.tail is tail
    assert m.parent is parent


@pytest.mark.asyncio
async def test_frontier_exhaustion_leaves_exit_captures_empty(build_block_tree):
    """Recursion ending by frontier exhaustion (no pure-exit fire): step lists
    populated, exit-branch captures stay []."""
    matcher, captured = _compile()
    parent = build_block_tree(BlockSpec(opcode=0x50, children=[
        BlockSpec(opcode=0x10, children=[
            BlockSpec(opcode=0x11, children=[            # Step1
                BlockSpec(opcode=0x11),                  # Step2, no children
            ]),
        ]),
    ]))
    anchor = parent.next_blocks[0]
    step1 = anchor.next_blocks[0]
    step2 = step1.next_blocks[0]

    assert await matcher.try_build(anchor) is not None
    m = captured[0]
    assert m.s == [step1, step2]
    assert m.exit_a == []
    assert m.exit_b == []
    assert m.tail is None
    assert m.parent is parent


# --------------------------------------------------------------------------
# maybe $rule absent / zero-hop, and stale-state isolation
# --------------------------------------------------------------------------

_SRC_MAYBE = """
opcode Anchor    = 0x10
opcode Step      = 0x11
opcode Note      = 0x12
opcode Exit      = 0x20
opcode ExitChild = 0x21
opcode Tail      = 0x40
opcode Parent    = 0x50

rule chain = (
    op Step @s -> { maybe op Note @note } -> maybe $chain
  ^ op Exit @exit_a -> op ExitChild @exit_b
)

matcher m {
    produces MB
    build    build_m
    entry    @anchor

    op Anchor @anchor
      -> { maybe $chain ; maybe op Tail @tail }
      <- op Parent @parent
}
"""


@pytest.mark.asyncio
async def test_maybe_rule_absent_yields_empty_lists_and_post_captures_bind(build_block_tree):
    """`maybe $rule` absent ⇒ every rule capture is [] (not None); captures after
    the absent recursion still bind (zero-hop shape)."""
    reg, captured = _registries_with(_OPCODES, "build_m", "MB")
    matcher = compile_text(_SRC_MAYBE, "test.mch", reg)[0].matcher

    parent = build_block_tree(BlockSpec(opcode=0x50, children=[
        BlockSpec(opcode=0x10, children=[BlockSpec(opcode=0x40)]),
    ]))
    anchor = parent.next_blocks[0]
    tail = anchor.next_blocks[0]

    assert await matcher.try_build(anchor) is not None
    m = captured[0]
    assert m.s == []
    assert m.note == []
    assert m.exit_a == []
    assert m.exit_b == []
    assert m.tail is tail
    assert m.parent is parent


@pytest.mark.asyncio
async def test_recursion_state_does_not_leak_across_matches(build_block_tree):
    """A multi-hop match followed by a recursion-absent match on the SAME
    compiled matcher must not replay the first match's iteration blocks
    (participation is checked against the current consumed list)."""
    reg, captured = _registries_with(_OPCODES, "build_m", "MB")
    matcher = compile_text(_SRC_MAYBE, "test.mch", reg)[0].matcher

    # Match 1: two-hop recursion.
    parent1 = build_block_tree(BlockSpec(opcode=0x50, children=[
        BlockSpec(opcode=0x10, children=[
            BlockSpec(opcode=0x11, children=[
                BlockSpec(opcode=0x11, children=[
                    BlockSpec(opcode=0x20, children=[BlockSpec(opcode=0x21)]),
                ]),
            ]),
        ]),
    ]))
    assert await matcher.try_build(parent1.next_blocks[0]) is not None
    assert len(captured[0].s) == 2

    # Match 2: recursion absent. Lists must be [], not match 1's leftovers.
    parent2 = build_block_tree(BlockSpec(opcode=0x50, children=[
        BlockSpec(opcode=0x10, children=[BlockSpec(opcode=0x40)]),
    ]))
    assert await matcher.try_build(parent2.next_blocks[0]) is not None
    m2 = captured[1]
    assert m2.s == []
    assert m2.note == []
    assert m2.exit_a == []
    assert m2.tail is parent2.next_blocks[0].next_blocks[0]
    assert m2.parent is parent2


# --------------------------------------------------------------------------
# Losing step-alternative branches (gap alignment across ^ branches)
# --------------------------------------------------------------------------

@pytest.mark.asyncio
async def test_step_alternative_losing_branch_contributes_none(build_block_tree):
    """Two step branches with distinct captures: each unrolling appends the
    winning branch's block and None for the losing branch — lists stay
    parallel-aligned (T18-b applied to alternatives, not just `maybe`)."""
    src = """
    opcode Anchor = 0x10
    opcode StepA  = 0x11
    opcode StepB  = 0x12

    rule chain = (
        op StepA @sa -> maybe $chain
      ^ op StepB @sb -> maybe $chain
    )

    matcher m {
        produces MB
        build    build_m
        entry    @anchor

        op Anchor @anchor -> $chain
    }
    """
    reg, captured = _registries_with(
        {"Anchor": 0x10, "StepA": 0x11, "StepB": 0x12}, "build_m", "MB")
    matcher = compile_text(src, "test.mch", reg)[0].matcher

    # Anchor -> A1 -> B2 -> A3
    anchor = build_block_tree(BlockSpec(opcode=0x10, children=[
        BlockSpec(opcode=0x11, children=[
            BlockSpec(opcode=0x12, children=[
                BlockSpec(opcode=0x11),
            ]),
        ]),
    ]))
    a1 = anchor.next_blocks[0]
    b2 = a1.next_blocks[0]
    a3 = b2.next_blocks[0]

    assert await matcher.try_build(anchor) is not None
    m = captured[0]
    assert m.sa == [a1, None, a3]
    assert m.sb == [None, b2, None]


@pytest.mark.asyncio
async def test_exit_alternative_losing_branch_contributes_none(build_block_tree):
    """An alternative inside the exit branch: on termination each exit capture
    gets exactly one element — the bound block for the winning sub-branch,
    None for the losing one."""
    src = """
    opcode Anchor = 0x10
    opcode Step   = 0x11
    opcode Exit   = 0x20
    opcode OutA   = 0x21
    opcode OutB   = 0x22

    rule chain = (
        op Step @s -> maybe $chain
      ^ op Exit @exit_head -> ( op OutA @out_a ^ op OutB @out_b )
    )

    matcher m {
        produces MB
        build    build_m
        entry    @anchor

        op Anchor @anchor -> $chain
    }
    """
    reg, captured = _registries_with(
        {"Anchor": 0x10, "Step": 0x11, "Exit": 0x20, "OutA": 0x21, "OutB": 0x22},
        "build_m", "MB")
    matcher = compile_text(src, "test.mch", reg)[0].matcher

    # Anchor -> Step1 -> Exit -> OutB
    anchor = build_block_tree(BlockSpec(opcode=0x10, children=[
        BlockSpec(opcode=0x11, children=[
            BlockSpec(opcode=0x20, children=[
                BlockSpec(opcode=0x22),
            ]),
        ]),
    ]))
    step1 = anchor.next_blocks[0]
    exit_ = step1.next_blocks[0]
    out_b = exit_.next_blocks[0]

    assert await matcher.try_build(anchor) is not None
    m = captured[0]
    assert m.s == [step1]
    assert m.exit_head == [exit_]
    assert m.out_a == [None]
    assert m.out_b == [out_b]


# --------------------------------------------------------------------------
# T16 stretch — consumed-set entry de-duplication
# --------------------------------------------------------------------------

@pytest.mark.asyncio
async def test_mch_consumed_is_deduplicated_order_preserving(build_block_tree):
    """The raw engine list reports the recursion's entry block twice; the
    synthesized build_block hands builders a first-occurrence-wins de-duplicated
    `_mch_consumed`, anchor first."""
    matcher, captured = _compile()
    (parent, anchor, step1, step2, step3, note2,
     tail, exit_, exit_child) = _three_hop_tree(build_block_tree)

    assert await matcher.try_build(anchor) is not None
    consumed = captured[0]._mch_consumed
    assert consumed[0] is anchor, "anchor must be first"
    assert len(consumed) == len({id(b) for b in consumed}), (
        f"consumed set must contain no duplicates, got {consumed!r}"
    )
    assert {id(b) for b in consumed} == {
        id(b) for b in (anchor, step1, step2, step3, note2, tail,
                        exit_, exit_child, parent)
    }
