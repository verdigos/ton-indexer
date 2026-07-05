"""T22: build-rejection contract, pinned in the IR engine (spec §10.7).

A build rejection — builder returns None, builder raises, or `reject when`
fires — must consume nothing, append no excess/bounce siblings, and leave the
anchor free for a later matcher (registration order) to try. Conventions
(fake blocks, registries, tree helpers) borrowed from test_engine.py.
"""
from __future__ import annotations

from types import SimpleNamespace

import pytest

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block
from indexer.events.mch.compile import compile_text
from indexer.events.mch.ir_emit import emit_ir
from indexer.events.mch.registry import Registries
from indexer.events.mch_ir import build_matchers, load_artifact

OP_FOO = 0xF00
OP_BAR = 0xBA7
EXCESS_OPCODE = 0xD53276DB
BOUNCE_OPCODE = 0xFFFFFFFF


class _FakeTx:
    def __init__(self, lt: int):
        self.lt = lt
        self.now = lt
        self.orig_status = "active"
        self.end_status = "active"


class _FakeMsg:
    def __init__(self, opcode: int, lt: int,
                 source: str = "0:" + "11" * 32, destination: str = "0:" + "22" * 32):
        self.opcode = opcode
        self.source = source
        self.destination = destination
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
    def __init__(self, opcode: int, lt: int = 10, **msg_kw):  # pylint: disable=super-init-not-called
        node = _FakeNode(_FakeMsg(opcode, lt, **msg_kw))
        Block.__init__(self, "call_contract", [node], None)
        self.opcode = opcode
        self.failed = False


def _link(parent: Block, child: Block) -> None:
    parent.next_blocks.append(child)
    child.previous_block = parent


def _registries() -> Registries:
    reg = Registries()
    reg.opcodes.update({"Foo": OP_FOO, "Bar": OP_BAR})
    reg.block_types["FB"] = object
    return reg


def _load(src: str, reg: Registries) -> list:
    compiled = compile_text(src, "test.mch", reg)
    art = load_artifact(emit_ir(compiled, ["test.mch"]))
    return build_matchers(art, reg)


def _foo_anchor_with_aux(lt: int = 10) -> Block:
    """anchor Foo -> excess sibling + bounce sibling (both call_contract)."""
    anchor = _FakeCC(OP_FOO, lt=lt)
    excess = _FakeCC(EXCESS_OPCODE, lt=lt + 1)
    bounce = _FakeCC(BOUNCE_OPCODE, lt=lt + 2)
    _link(anchor, excess)
    _link(anchor, bounce)
    return anchor, excess, bounce


# ==========================================================================
# 1. Rejecting matcher yields the anchor to the next matcher in order
# ==========================================================================

_TWO_MATCHERS_SRC = """
matcher rejects_first {
    produces FB
    build    reject_build
    entry    @x
    op Foo @x
}

matcher accepts_second {
    produces FB
    build    accept_build
    entry    @x
    op Foo @x
}
"""


@pytest.mark.asyncio
async def test_rejecting_matcher_lets_next_matcher_consume_same_anchor():
    async def reject_build(_m):
        return None

    async def accept_build(m):
        return Block("fb", [], {"ok": True})

    reg = _registries()
    reg.builders["reject_build"] = reject_build
    reg.builders["accept_build"] = accept_build
    first, second = _load(_TWO_MATCHERS_SRC, reg)
    assert first.name == "rejects_first"
    assert second.name == "accepts_second"

    anchor = _FakeCC(OP_FOO, lt=10)

    result_first = await first.try_build(anchor)
    assert result_first == []
    assert anchor.parent is None  # nothing consumed by the rejecting matcher

    result_second = await second.try_build(anchor)
    assert len(result_second) == 1
    assert result_second[0].data == {"ok": True}
    assert anchor.parent is result_second[0]


# ==========================================================================
# 2. Rejection with excess/bounce siblings present: clean [], no auto-append
# ==========================================================================

@pytest.mark.asyncio
async def test_rejection_with_excess_and_bounce_children_stays_clean():
    src = """
    matcher via_builder {
        produces FB
        build    my_build
        entry    @x
        op Foo @x
    }
    """

    async def my_build(_m):
        return None

    reg = _registries()
    reg.builders["my_build"] = my_build
    (m,) = _load(src, reg)

    anchor, excess, bounce = _foo_anchor_with_aux()
    result = await m.try_build(anchor)
    assert result == []
    assert anchor.parent is None
    assert excess.parent is None
    assert bounce.parent is None
    # Aux siblings remain in place on the anchor, still matchable.
    assert excess in anchor.next_blocks
    assert bounce in anchor.next_blocks


# ==========================================================================
# 3. Builder raising is treated as a clean rejection, not a crash
# ==========================================================================

@pytest.mark.asyncio
async def test_builder_raising_is_clean_rejection_no_crash(caplog):
    src = """
    matcher via_builder {
        produces FB
        build    my_build
        entry    @x
        op Foo @x
    }
    """

    async def my_build(_m):
        raise RuntimeError("boom")

    reg = _registries()
    reg.builders["my_build"] = my_build
    (m,) = _load(src, reg)

    anchor, excess, bounce = _foo_anchor_with_aux()
    with caplog.at_level("ERROR", logger="indexer.events.mch_ir.engine"):
        result = await m.try_build(anchor)  # must not raise
    assert result == []
    assert anchor.parent is None
    assert excess.parent is None
    assert bounce.parent is None
    assert any("boom" in r.message for r in caplog.records)


# ==========================================================================
# 4. Declarative `reject when` — same clean semantics
# ==========================================================================

@pytest.mark.asyncio
async def test_declarative_reject_when_stays_clean_with_aux_children():
    src = """
    matcher decl_reject {
        produces FB
        entry    @x
        op Foo @x
        reject when true
        out { v: 1 }
    }
    """
    reg = _registries()
    (m,) = _load(src, reg)

    anchor, excess, bounce = _foo_anchor_with_aux()
    result = await m.try_build(anchor)
    assert result == []
    assert anchor.parent is None
    assert excess.parent is None
    assert bounce.parent is None
    assert excess in anchor.next_blocks
    assert bounce in anchor.next_blocks
