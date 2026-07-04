"""Adversarial walker tests, R1-R9 from docs/mch-engine-v2.md §8.

Each R-case pins one legacy-parity trap of the v2 purpose-built walker:
reentrancy (R1), determinism (R2), rollback (R3), child-edge multi-match
accumulation (R4), children-set semantics (R5), recursion frontier + capture
alignment (R6 — the 11 tests/mch/test_recursion_captures.py cases run through
the IR path), rejection-as-return-value (R7), two-level excess/bounce handling
(R8), and the exclusive-or head-test gate (R9). Plus the two new load-time
validations (unknown parse message type, nested recursion).

Matchers come from the real frontend (compile_text -> emit_ir -> load ->
build_matchers) except where a shape the frontend cannot emit is needed
(child+children overlap, overlapping exclusive heads, nested recursion) —
those use hand-crafted artifact dicts against the 0.1-poc schema.
"""
from __future__ import annotations

from types import SimpleNamespace

import pytest

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block
from indexer.events.mch.compile import compile_text
from indexer.events.mch.ir_emit import emit_ir
from indexer.events.mch.registry import Registries
from indexer.events.mch_ir import IrLoadError, build_matchers, load_artifact

OP_FOO = 0xF00
OP_BAR = 0xBA7
OP_BAZ = 0xBA2
OP_QUX = 0x0F1
OP_EXCESS = 0xD53276DB
OP_BOUNCE = 0xFFFFFFFF


# ==========================================================================
# Synthetic call_contract blocks (trimmed copy of the test_engine fixtures:
# fake event nodes give real lt/utime bookkeeping so merge_blocks works).
# ==========================================================================

class _FakeTx:
    def __init__(self, lt: int):
        self.lt = lt
        self.now = lt
        self.orig_status = "active"
        self.end_status = "active"


class _FakeMsg:
    def __init__(self, opcode: int, lt: int):
        self.opcode = opcode
        self.source = "0:" + "11" * 32
        self.destination = "0:" + "22" * 32
        self.value = 1000
        self.direction = "in"
        self.created_lt = lt
        self.created_at = lt
        self.message_content = SimpleNamespace(body=None)
        self.transaction = _FakeTx(lt)


class _FakeNode:
    is_tick_tock = False
    tick_tock_tx = None
    ghost_node = False
    parent = None

    def __init__(self, msg: _FakeMsg):
        self.message = msg

    def get_lt(self):
        return self.message.created_lt

    def get_utime(self):
        return self.message.created_at

    def get_opcode(self):
        return self.message.opcode & 0xFFFFFFFF


class _FakeCC(CallContractBlock):
    def __init__(self, opcode: int, lt: int):  # pylint: disable=super-init-not-called
        Block.__init__(self, "call_contract", [_FakeNode(_FakeMsg(opcode, lt))], None)
        self.opcode = opcode & 0xFFFFFFFF


_LT = [0]


def _cc(opcode: int) -> _FakeCC:
    # Creation-order lts: build trees top-down so the anchor is the earliest
    # block (merge_blocks requirement on success paths).
    _LT[0] += 10
    return _FakeCC(opcode, lt=_LT[0])


def _link(parent: Block, child: Block) -> None:
    parent.next_blocks.append(child)
    child.previous_block = parent


# ==========================================================================
# Helpers
# ==========================================================================

def _registries() -> Registries:
    reg = Registries()
    reg.opcodes.update({
        "Foo": OP_FOO, "Bar": OP_BAR, "Baz": OP_BAZ, "Qux": OP_QUX,
    })
    reg.block_types["FB"] = object
    return reg


def _ir(src: str, reg: Registries):
    """Frontend-compile `src`, emit its IR, wire it through the v2 engine."""
    compiled = compile_text(src, "test.mch", reg)
    art = load_artifact(emit_ir(compiled, ["test.mch"]))
    return build_matchers(art, reg)[0]


def _grab_registries() -> tuple[Registries, list]:
    """Registries with a `grab` builder that records the match and produces a
    bare block (success path with observable captures/consumed)."""
    seen: list = []
    reg = _registries()

    async def grab(m):
        seen.append(m)
        return Block("fb", [], None)

    reg.builders["grab"] = grab
    return reg, seen


def _artifact(nodes: list[dict], matcher: dict) -> dict:
    return {
        "mch_ir_version": "0.1-poc",
        "frontend": {"generator": "test", "source_files": []},
        "matchers": [matcher],
        "nodes": nodes,
        "registration_order": [0],
    }


def _slot_snapshot(obj) -> list[tuple[str, int]]:
    names: list[str] = []
    for cls in type(obj).__mro__:
        names.extend(getattr(cls, "__slots__", ()))
    return [(n, id(getattr(obj, n))) for n in names]


# ==========================================================================
# R6 fixtures: the recursion-capture conformance sources (T18), verbatim from
# tests/mch/test_recursion_captures.py, run through the IR round trip.
# ==========================================================================

_REC_SRC = """
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

_REC_SRC_MAYBE = """
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


def _rec_registries() -> tuple[Registries, list]:
    """Registries for the recursion sources: rejecting builder that records the
    match (rejection keeps the tree unmutated, so it can be re-walked)."""
    captured: list = []
    reg = Registries()
    reg.block_types["MB"] = object

    async def build_m(m):
        captured.append(m)
        return None

    reg.builders["build_m"] = build_m
    return reg, captured


def _rec_matcher(src: str = _REC_SRC):
    reg, captured = _rec_registries()
    return _ir(src, reg), captured


def _three_hop_tree():
    """Parent(Anchor(Step1(Step2(Note2, Step3(Exit(ExitChild)))), Tail))."""
    parent = _cc(0x50)
    anchor = _cc(0x10)
    step1 = _cc(0x11)
    step2 = _cc(0x11)
    note2 = _cc(0x12)
    step3 = _cc(0x11)
    exit_ = _cc(0x20)
    exit_child = _cc(0x21)
    tail = _cc(0x40)
    _link(parent, anchor)
    _link(anchor, step1)
    _link(anchor, tail)
    _link(step1, step2)
    _link(step2, note2)
    _link(step2, step3)
    _link(step3, exit_)
    _link(exit_, exit_child)
    return parent, anchor, step1, step2, step3, note2, tail, exit_, exit_child


# ==========================================================================
# R1 — reentrancy: no state bleed across matches; matcher immutable after load
# ==========================================================================

@pytest.mark.asyncio
async def test_r1_reentrancy_no_state_bleed_and_immutable_matcher():
    matcher, captured = _rec_matcher(_REC_SRC_MAYBE)
    assert not hasattr(matcher, "__dict__"), "matcher must carry no mutable attrs"
    snapshot = _slot_snapshot(matcher)

    # Match 1: two-hop recursion.
    parent1 = _cc(0x50)
    anchor1 = _cc(0x10)
    s1 = _cc(0x11)
    s2 = _cc(0x11)
    e1 = _cc(0x20)
    ec1 = _cc(0x21)
    _link(parent1, anchor1)
    _link(anchor1, s1)
    _link(s1, s2)
    _link(s2, e1)
    _link(e1, ec1)
    assert await matcher.try_build(anchor1) is not None
    assert len(captured[0].s) == 2

    # Match 2 on the SAME matcher: recursion absent -> lists must be fresh,
    # not match 1's leftovers.
    parent2 = _cc(0x50)
    anchor2 = _cc(0x10)
    tail2 = _cc(0x40)
    _link(parent2, anchor2)
    _link(anchor2, tail2)
    assert await matcher.try_build(anchor2) is not None
    m2 = captured[1]
    assert m2.s == [] and m2.note == []
    assert m2.exit_a == [] and m2.exit_b == []
    assert m2.tail is tail2 and m2.parent is parent2

    assert _slot_snapshot(matcher) == snapshot, "try_build rebound a matcher attr"


# ==========================================================================
# R2 — determinism: identical runs produce identical bindings + consumed lists
# ==========================================================================

@pytest.mark.asyncio
async def test_r2_two_identical_runs_bind_identically():
    matcher, captured = _rec_matcher()
    tree = _three_hop_tree()
    anchor = tree[1]

    assert await matcher.try_build(anchor) == []  # rejecting builder: tree untouched
    assert await matcher.try_build(anchor) == []
    m1, m2 = captured

    for field in ("s", "note", "exit_a", "exit_b"):
        v1, v2 = getattr(m1, field), getattr(m2, field)
        assert [id(b) if b is not None else None for b in v1] \
            == [id(b) if b is not None else None for b in v2], field
    for field in ("anchor", "direct", "tail", "parent"):
        assert getattr(m1, field) is getattr(m2, field), field
    assert [id(b) for b in m1._mch_consumed] == [id(b) for b in m2._mch_consumed]


# ==========================================================================
# R3 — rollback: a partially-consuming or-branch that fails leaves no trace
# ==========================================================================

@pytest.mark.asyncio
async def test_r3_failed_or_branch_rolls_back_consumed_and_bindings():
    reg, seen = _grab_registries()
    src = """
    matcher r3 {
        produces FB
        build    grab
        entry    @x

        op Foo @x -> { ( op Bar @a -> op Baz @b -> op Qux @q | op Bar @c ) }
    }
    """
    matcher = _ir(src, reg)

    foo = _cc(OP_FOO)
    bar = _cc(OP_BAR)
    baz = _cc(OP_BAZ)  # branch 1 consumes bar+baz, then fails on the missing Qux
    _link(foo, bar)
    _link(bar, baz)

    (produced,) = await matcher.try_build(foo)
    m = seen[0]
    assert m.a is None and m.b is None and m.q is None, "losing branch bindings leaked"
    assert m.c is bar
    assert [id(b) for b in m._mch_consumed] == [id(foo), id(bar)], (
        "consumed must hold exactly the winning branch (visited/consumed rollback)"
    )
    assert bar.parent is produced
    assert baz.parent is None, "rolled-back block must not be absorbed"


# ==========================================================================
# R4 — child edge: multi-match accumulation, scalar capture = first binding
# ==========================================================================

@pytest.mark.asyncio
async def test_r4_child_edge_consumes_all_matches_scalar_takes_first():
    reg, seen = _grab_registries()
    src = """
    matcher r4 {
        produces FB
        build    grab
        entry    @x

        op Foo @x -> op Bar @y
    }
    """
    matcher = _ir(src, reg)

    foo = _cc(OP_FOO)
    bar1 = _cc(OP_BAR)
    bar2 = _cc(OP_BAR)
    _link(foo, bar1)
    _link(foo, bar2)

    (produced,) = await matcher.try_build(foo)
    m = seen[0]
    assert m.y is bar1, "scalar capture must take the FIRST matching child"
    assert [id(b) for b in m._mch_consumed] == [id(foo), id(bar1), id(bar2)], (
        "both matching children must be consumed (not first-win)"
    )
    assert bar1.parent is produced and bar2.parent is produced


# ==========================================================================
# R5 — children set
# ==========================================================================

@pytest.mark.asyncio
async def test_r5_children_set_declaration_order_and_working_set_removal():
    reg, seen = _grab_registries()
    src = """
    matcher r5 {
        produces FB
        build    grab
        entry    @x

        op Foo @x -> { op Bar @a ; op Bar @b }
    }
    """
    matcher = _ir(src, reg)

    foo = _cc(OP_FOO)
    bar1 = _cc(OP_BAR)
    bar2 = _cc(OP_BAR)
    _link(foo, bar1)
    _link(foo, bar2)

    assert len(await matcher.try_build(foo)) == 1
    m = seen[0]
    # Item 1 takes the first candidate and removes it from the working set;
    # item 2 must land on the second.
    assert m.a is bar1 and m.b is bar2


@pytest.mark.asyncio
async def test_r5_required_item_unmatched_fails():
    reg, seen = _grab_registries()
    src = """
    matcher r5b {
        produces FB
        build    grab
        entry    @x

        op Foo @x -> { op Bar @a ; op Bar @b }
    }
    """
    matcher = _ir(src, reg)

    foo = _cc(OP_FOO)
    bar1 = _cc(OP_BAR)
    _link(foo, bar1)

    assert await matcher.try_build(foo) is None
    assert seen == [] and foo.parent is None and bar1.parent is None


@pytest.mark.asyncio
async def test_r5_optional_tail_item_passes():
    reg, seen = _grab_registries()
    src = """
    matcher r5c {
        produces FB
        build    grab
        entry    @x

        op Foo @x -> { op Bar @a ; maybe op Baz @c }
    }
    """
    matcher = _ir(src, reg)

    foo = _cc(OP_FOO)
    bar = _cc(OP_BAR)
    _link(foo, bar)

    assert len(await matcher.try_build(foo)) == 1
    assert seen[0].a is bar and seen[0].c is None


@pytest.mark.asyncio
async def test_r5_overlap_with_child_edge_dedups():
    # child + children on the same node is not expressible in .mch source;
    # hand-craft the artifact (legal per the schema).
    art = _artifact(
        nodes=[
            {"kind": "contract", "opcode": OP_FOO, "capture": "x",
             "child": 1, "children": [2]},
            {"kind": "contract", "opcode": OP_BAR, "capture": "y"},
            {"kind": "contract", "opcode": OP_BAR, "capture": "z"},
        ],
        matcher={
            "name": "overlap",
            "anchor": {"kind": "opcode_set", "values": [OP_FOO]},
            "root": 0,
            "produces": ["fb"],
            "captures": [{"name": "x", "card": "one"}, {"name": "y", "card": "one"},
                         {"name": "z", "card": "one"}],
            "builder": "grab",
        },
    )
    reg, seen = _grab_registries()
    matcher = build_matchers(load_artifact(art), reg)[0]

    foo = _cc(OP_FOO)
    bar = _cc(OP_BAR)
    _link(foo, bar)

    assert len(await matcher.try_build(foo)) == 1
    m = seen[0]
    assert m.y is bar and m.z is bar, "children set may re-match a child-edge block"
    assert [id(b) for b in m._mch_consumed] == [id(foo), id(bar)], (
        "overlapping consumption must identity-dedup"
    )


# ==========================================================================
# R6 — recursion frontier + capture alignment: the 11 T18 conformance cases
# (tests/mch/test_recursion_captures.py) through the IR path.
# ==========================================================================

@pytest.mark.asyncio
async def test_r6_three_hop_step_captures_parallel_aligned_trace_order():
    matcher, captured = _rec_matcher()
    (parent, anchor, step1, step2, step3, note2,
     tail, exit_, exit_child) = _three_hop_tree()

    assert await matcher.try_build(anchor) is not None
    m = captured[0]
    assert m.anchor is anchor
    assert m.s == [step1, step2, step3]
    assert m.note == [None, note2, None]
    assert len(m.note) == len(m.s)
    assert m.s[-1] is step3


@pytest.mark.asyncio
async def test_r6_exit_branch_captures_bind_once_no_padding():
    matcher, captured = _rec_matcher()
    (parent, anchor, step1, step2, step3, note2,
     tail, exit_, exit_child) = _three_hop_tree()

    assert await matcher.try_build(anchor) is not None
    m = captured[0]
    assert m.exit_a == [exit_]
    assert m.exit_b == [exit_child]


@pytest.mark.asyncio
async def test_r6_post_recursion_captures_bind_on_multi_hop():
    matcher, captured = _rec_matcher()
    (parent, anchor, step1, step2, step3, note2,
     tail, exit_, exit_child) = _three_hop_tree()

    assert await matcher.try_build(anchor) is not None
    m = captured[0]
    assert m.tail is tail, "children-set item after the recursion must bind"
    assert m.parent is parent, "parent capture after the recursion must bind"
    assert m.direct is None, "losing alternative branch must stay None"


@pytest.mark.asyncio
async def test_r6_single_hop_non_recursive_alternative_binds_post_captures():
    matcher, captured = _rec_matcher()
    parent = _cc(0x50)
    anchor = _cc(0x10)
    direct = _cc(0x30)
    tail = _cc(0x40)
    _link(parent, anchor)
    _link(anchor, direct)
    _link(anchor, tail)

    assert await matcher.try_build(anchor) is not None
    m = captured[0]
    assert m.direct is direct
    assert m.tail is tail
    assert m.parent is parent
    assert m.s == [] and m.note == []
    assert m.exit_a == [] and m.exit_b == []


@pytest.mark.asyncio
async def test_r6_one_hop_recursion_shape():
    matcher, captured = _rec_matcher()
    parent = _cc(0x50)
    anchor = _cc(0x10)
    step1 = _cc(0x11)
    exit_ = _cc(0x20)
    exit_child = _cc(0x21)
    tail = _cc(0x40)
    _link(parent, anchor)
    _link(anchor, step1)
    _link(anchor, tail)
    _link(step1, exit_)
    _link(exit_, exit_child)

    assert await matcher.try_build(anchor) is not None
    m = captured[0]
    assert m.s == [step1]
    assert m.note == [None]
    assert m.exit_a == [exit_]
    assert m.exit_b == [exit_child]
    assert m.tail is tail
    assert m.parent is parent


@pytest.mark.asyncio
async def test_r6_frontier_exhaustion_leaves_exit_captures_empty():
    """Recursion ending by frontier exhaustion still succeeds after >=1 step
    even though the rule declares a pure exit (legacy rule, NOT spec §4.6's
    exit-declared-failure — see engine module docstring)."""
    matcher, captured = _rec_matcher()
    parent = _cc(0x50)
    anchor = _cc(0x10)
    step1 = _cc(0x11)
    step2 = _cc(0x11)
    _link(parent, anchor)
    _link(anchor, step1)
    _link(step1, step2)

    assert await matcher.try_build(anchor) is not None
    m = captured[0]
    assert m.s == [step1, step2]
    assert m.exit_a == [] and m.exit_b == []
    assert m.tail is None
    assert m.parent is parent


@pytest.mark.asyncio
async def test_r6_maybe_rule_absent_yields_empty_lists_and_post_captures_bind():
    matcher, captured = _rec_matcher(_REC_SRC_MAYBE)
    parent = _cc(0x50)
    anchor = _cc(0x10)
    tail = _cc(0x40)
    _link(parent, anchor)
    _link(anchor, tail)

    assert await matcher.try_build(anchor) is not None
    m = captured[0]
    assert m.s == [] and m.note == []
    assert m.exit_a == [] and m.exit_b == []
    assert m.tail is tail
    assert m.parent is parent


@pytest.mark.asyncio
async def test_r6_recursion_state_does_not_leak_across_matches():
    matcher, captured = _rec_matcher(_REC_SRC_MAYBE)

    parent1 = _cc(0x50)
    anchor1 = _cc(0x10)
    s1 = _cc(0x11)
    s2 = _cc(0x11)
    e1 = _cc(0x20)
    ec1 = _cc(0x21)
    _link(parent1, anchor1)
    _link(anchor1, s1)
    _link(s1, s2)
    _link(s2, e1)
    _link(e1, ec1)
    assert await matcher.try_build(anchor1) is not None
    assert len(captured[0].s) == 2

    parent2 = _cc(0x50)
    anchor2 = _cc(0x10)
    tail2 = _cc(0x40)
    _link(parent2, anchor2)
    _link(anchor2, tail2)
    assert await matcher.try_build(anchor2) is not None
    m2 = captured[1]
    assert m2.s == [] and m2.note == [] and m2.exit_a == []
    assert m2.tail is tail2
    assert m2.parent is parent2


@pytest.mark.asyncio
async def test_r6_step_alternative_losing_branch_contributes_none():
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
    reg, captured = _rec_registries()
    matcher = _ir(src, reg)

    anchor = _cc(0x10)
    a1 = _cc(0x11)
    b2 = _cc(0x12)
    a3 = _cc(0x11)
    _link(anchor, a1)
    _link(a1, b2)
    _link(b2, a3)

    assert await matcher.try_build(anchor) is not None
    m = captured[0]
    assert m.sa == [a1, None, a3]
    assert m.sb == [None, b2, None]


@pytest.mark.asyncio
async def test_r6_exit_alternative_losing_branch_contributes_none():
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
    reg, captured = _rec_registries()
    matcher = _ir(src, reg)

    anchor = _cc(0x10)
    step1 = _cc(0x11)
    exit_ = _cc(0x20)
    out_b = _cc(0x22)
    _link(anchor, step1)
    _link(step1, exit_)
    _link(exit_, out_b)

    assert await matcher.try_build(anchor) is not None
    m = captured[0]
    assert m.s == [step1]
    assert m.exit_head == [exit_]
    assert m.out_a == [None]
    assert m.out_b == [out_b]


@pytest.mark.asyncio
async def test_r6_mch_consumed_is_deduplicated_order_preserving():
    matcher, captured = _rec_matcher()
    (parent, anchor, step1, step2, step3, note2,
     tail, exit_, exit_child) = _three_hop_tree()

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


# ==========================================================================
# R7 — rejection is a return value: [], no aux attached, matcher reusable
# ==========================================================================

@pytest.mark.asyncio
async def test_r7_builder_rejection_no_aux_and_matcher_reusable():
    reg = _registries()
    state = {"reject": True}
    seen: list = []

    async def grab(m):
        seen.append(m)
        return None if state["reject"] else Block("fb", [], None)

    reg.builders["grab"] = grab
    src = """
    matcher r7 {
        produces FB
        build    grab
        entry    @x

        op Foo @x -> op Bar @y
    }
    """
    matcher = _ir(src, reg)

    foo = _cc(OP_FOO)
    bar = _cc(OP_BAR)
    exc = _cc(OP_EXCESS)
    _link(foo, bar)
    _link(foo, exc)

    assert await matcher.try_build(foo) == [], "rejection must not attach aux blocks"
    assert foo.parent is None and bar.parent is None and exc.parent is None

    assert await matcher.try_build(foo) == []          # reusable after rejection
    state["reject"] = False
    result = await matcher.try_build(foo)              # ...and can still succeed
    assert len(result) == 2 and result[1] is exc
    assert bar.parent is result[0]


@pytest.mark.asyncio
async def test_r7_reject_when_returns_empty_no_aux():
    src = """
    matcher r7d {
        produces FB
        entry    @x

        op Foo @x

        reject when 1 == 1
        out { v: 1 }
    }
    """
    matcher = _ir(src, _registries())

    foo = _cc(OP_FOO)
    exc = _cc(OP_EXCESS)
    _link(foo, exc)

    assert await matcher.try_build(foo) == []
    assert foo.parent is None and exc.parent is None


# ==========================================================================
# R8 — excess/bounce: nested absorption into consumed vs root append-to-result
# ==========================================================================

@pytest.mark.asyncio
async def test_r8_nested_absorption_merges_aux_into_consumed():
    reg, seen = _grab_registries()
    src = """
    matcher r8 {
        produces FB
        build    grab
        entry    @x

        op Foo @x -> op Bar @y
    }
    """
    matcher = _ir(src, reg)

    foo = _cc(OP_FOO)
    bar = _cc(OP_BAR)
    exc = _cc(OP_EXCESS)  # child of the MATCHED NESTED node -> absorbed
    _link(foo, bar)
    _link(bar, exc)

    result = await matcher.try_build(foo)
    assert len(result) == 1, "nested aux must not be appended to the result"
    assert [id(b) for b in seen[0]._mch_consumed] == [id(foo), id(bar), id(exc)]
    assert exc.parent is result[0], "nested aux must be merged into the produced block"


@pytest.mark.asyncio
async def test_r8_root_aux_appended_to_result_not_merged():
    reg, seen = _grab_registries()
    src = """
    matcher r8b {
        produces FB
        build    grab
        entry    @x

        op Foo @x -> op Bar @y
    }
    """
    matcher = _ir(src, reg)

    foo = _cc(OP_FOO)
    bar = _cc(OP_BAR)
    exc = _cc(OP_EXCESS)
    bounce = _cc(OP_BOUNCE)
    _link(foo, bar)
    _link(foo, exc)
    _link(foo, bounce)

    result = await matcher.try_build(foo)
    assert result[1] is exc and result[2] is bounce and len(result) == 3
    assert exc.parent is None and bounce.parent is None, (
        "root-level aux is appended to the result, never merged"
    )
    assert [id(b) for b in seen[0]._mch_consumed] == [id(foo), id(bar)]


@pytest.mark.asyncio
async def test_r8_include_flags_false_respected_at_root_only():
    reg, seen = _grab_registries()
    src = """
    matcher r8c {
        produces FB
        build    grab
        entry    @x

        op Foo @x -> op Bar @y
    }
    """
    compiled = compile_text(src, "test.mch", reg)
    art = emit_ir(compiled, ["test.mch"])
    art["matchers"][0]["include_excess"] = False
    art["matchers"][0]["include_bounces"] = False
    matcher = build_matchers(load_artifact(art), reg)[0]

    foo = _cc(OP_FOO)
    bar = _cc(OP_BAR)
    root_exc = _cc(OP_EXCESS)
    root_bounce = _cc(OP_BOUNCE)
    nested_exc = _cc(OP_EXCESS)
    _link(foo, bar)
    _link(foo, root_exc)
    _link(foo, root_bounce)
    _link(bar, nested_exc)

    result = await matcher.try_build(foo)
    assert len(result) == 1, "flags off: nothing appended at the root"
    assert root_exc.parent is None and root_bounce.parent is None
    # Nested absorption is NOT governed by the record flags (legacy nested
    # matchers always carry default-true flags).
    assert nested_exc.parent is result[0]


# ==========================================================================
# R9 — exclusive-or: the count gate lives in the HEAD TEST only (legacy
# ExclusiveOrMatcher overrides test_self, which try_build never consults).
# As an edge target the node behaves exactly like plain or, first-full-build
# wins — specs/stonfi_v2_swap.mch relies on this for its overlapping `^`
# branches. The gate IS observable where head tests are consulted: the
# recursion entry test.
# ==========================================================================

def _xor_artifact(exclusive: bool, branches: list[dict]) -> dict:
    nodes = [
        {"kind": "contract", "opcode": OP_FOO, "child": 1},
        {"kind": "or", "exclusive": exclusive,
         "branches": list(range(2, 2 + len(branches)))},
    ] + branches
    return _artifact(nodes, {
        "name": "xor",
        "anchor": {"kind": "opcode_set", "values": [OP_FOO]},
        "root": 0,
        "produces": ["fb"],
        "captures": [{"name": "a", "card": "one"}, {"name": "b", "card": "one"}],
        "builder": "grab",
    })


@pytest.mark.asyncio
async def test_r9_exclusive_edge_target_is_gate_inert_first_match_wins():
    branches = [
        {"kind": "contract", "opcode": OP_BAR, "capture": "a"},
        {"kind": "pred", "pred": "always", "capture": "b"},
    ]
    reg, seen = _grab_registries()
    reg.predicates["always"] = lambda _b: True

    for exclusive in (True, False):
        matcher = build_matchers(load_artifact(_xor_artifact(exclusive, branches)), reg)[0]
        # Both branch heads match -> as an edge target the exclusive form must
        # behave exactly like plain or: first full build wins, no count gate.
        foo = _cc(OP_FOO)
        bar = _cc(OP_BAR)
        _link(foo, bar)
        assert len(await matcher.try_build(foo)) == 1, f"exclusive={exclusive}"
        m = seen[-1]
        assert m.a is bar and m.b is None, "first branch in order must win"
        # Exactly one head matches -> that branch wins on both forms.
        foo2 = _cc(OP_FOO)
        qux = _cc(OP_QUX)
        _link(foo2, qux)
        assert len(await matcher.try_build(foo2)) == 1
        assert seen[-1].a is None and seen[-1].b is qux


@pytest.mark.asyncio
async def test_r9_zero_heads_fail():
    branches = [
        {"kind": "contract", "opcode": OP_BAR, "capture": "a"},
        {"kind": "contract", "opcode": OP_BAZ, "capture": "b"},
    ]
    reg, seen = _grab_registries()
    for exclusive in (True, False):
        matcher = build_matchers(load_artifact(_xor_artifact(exclusive, branches)), reg)[0]
        foo = _cc(OP_FOO)
        qux = _cc(OP_QUX)
        _link(foo, qux)
        assert await matcher.try_build(foo) is None
        assert seen == []


@pytest.mark.asyncio
async def test_r9_single_head_full_build_failure_behaves_like_or():
    # Branch 1's head matches but its subtree fails; branch 2's head never
    # matches -> both the gated and the plain form fail identically.
    branches = [
        {"kind": "contract", "opcode": OP_BAR, "capture": "a", "child": 4},
        {"kind": "contract", "opcode": OP_BAZ, "capture": "b"},
    ]
    extra = {"kind": "contract", "opcode": OP_QUX}
    reg, seen = _grab_registries()
    for exclusive in (True, False):
        art = _xor_artifact(exclusive, branches)
        art["nodes"].append(extra)
        matcher = build_matchers(load_artifact(art), reg)[0]
        foo = _cc(OP_FOO)
        bar = _cc(OP_BAR)  # no Qux child -> branch 1 subtree fails
        _link(foo, bar)
        assert await matcher.try_build(foo) is None
        assert seen == [] and bar.parent is None


@pytest.mark.asyncio
async def test_r9_exclusive_gate_applies_at_recursion_head_test():
    """The count gate fires where legacy consulted test_self: an exclusive-or
    recursion STEP with two matching branch heads fails the entry test, so the
    (optional) recursion never participates; the plain-or form steps."""
    def art(exclusive: bool) -> dict:
        return _artifact(
            nodes=[
                {"kind": "contract", "opcode": OP_FOO, "child": 1},
                {"kind": "recursive", "step": 2, "optional": True},
                {"kind": "or", "exclusive": exclusive, "branches": [3, 4]},
                {"kind": "contract", "opcode": OP_BAR, "capture": "a"},
                {"kind": "pred", "pred": "always", "capture": "b"},
            ],
            matcher={
                "name": "recgate",
                "anchor": {"kind": "opcode_set", "values": [OP_FOO]},
                "root": 0,
                "produces": ["fb"],
                "captures": [{"name": "a", "card": "one"}, {"name": "b", "card": "one"}],
                "builder": "grab",
            },
        )

    reg, seen = _grab_registries()
    reg.predicates["always"] = lambda _b: True

    xm = build_matchers(load_artifact(art(True)), reg)[0]
    foo = _cc(OP_FOO)
    bar = _cc(OP_BAR)
    _link(foo, bar)
    assert len(await xm.try_build(foo)) == 1
    assert seen[0].a is None and seen[0].b is None, "gated entry: recursion absent"
    assert [id(b) for b in seen[0]._mch_consumed] == [id(foo)]

    om = build_matchers(load_artifact(art(False)), reg)[0]
    foo2 = _cc(OP_FOO)
    bar2 = _cc(OP_BAR)
    _link(foo2, bar2)
    assert len(await om.try_build(foo2)) == 1
    assert seen[1].a is bar2, "plain-or step must fire the recursion"
    assert [id(b) for b in seen[1]._mch_consumed] == [id(foo2), id(bar2)]


# ==========================================================================
# Load-time validation added in v2 (spec §2)
# ==========================================================================

def test_load_rejects_unknown_parse_message_type():
    art = _artifact(
        nodes=[{"kind": "contract", "opcode": OP_FOO, "capture": "x"}],
        matcher={
            "name": "p",
            "anchor": {"kind": "opcode_set", "values": [OP_FOO]},
            "root": 0,
            "produces": ["fb"],
            "captures": [{"name": "x", "card": "one"}],
            "build_program": [
                {"s": "parse", "target": "x", "types": ["NoSuchType"]},
                {"s": "out", "fields": []},
            ],
        },
    )
    with pytest.raises(IrLoadError, match="NoSuchType"):
        build_matchers(load_artifact(art), _registries())


def test_load_rejects_nested_recursion():
    art = _artifact(
        nodes=[
            {"kind": "contract", "opcode": OP_FOO, "child": 1},
            {"kind": "recursive", "step": 2, "optional": True},
            {"kind": "contract", "opcode": OP_BAR, "child": 3},
            {"kind": "recursive", "step": 4, "optional": True},
            {"kind": "contract", "opcode": OP_BAZ},
        ],
        matcher={
            "name": "nested",
            "anchor": {"kind": "opcode_set", "values": [OP_FOO]},
            "root": 0,
            "produces": ["fb"],
        },
    )
    with pytest.raises(IrLoadError, match="nested recursion"):
        build_matchers(load_artifact(art), _registries())
