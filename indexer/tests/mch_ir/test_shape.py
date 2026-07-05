"""R06 `shape` directive through the IR path: loader validation, load-time
shaper check, engine post-merge invocation, and topology parity with the
compiled path (spec §8.7, docs/mch-ir-poc.md matcher record).
"""
from __future__ import annotations

from types import SimpleNamespace

import pytest

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block, EmptyBlock
from indexer.events.mch.compile import compile_text
from indexer.events.mch.ir_emit import emit_ir
from indexer.events.mch.registry import Registries
from indexer.events.mch_ir import build_matchers, load_artifact
from indexer.events.mch_ir.loader import IrLoadError

OP_A = 0x1
OP_B = 0x2


# ==========================================================================
# Synthetic blocks with fake event nodes (merge_blocks-capable)
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
    def __init__(self, opcode: int, lt: int = 10):  # pylint: disable=super-init-not-called
        Block.__init__(self, "call_contract", [_FakeNode(_FakeMsg(opcode, lt))], None)
        self.opcode = opcode


def _ab_tree() -> tuple[Block, Block]:
    a = _FakeCC(OP_A, lt=10)
    b = _FakeCC(OP_B, lt=20)
    a.next_blocks.append(b)
    b.previous_block = a
    return a, b


SRC_SHAPED = """
opcode A = 0x1
opcode B = 0x2

matcher shaped {
    produces MB
    build    build_shaped
    shape    insert_proxy
    entry    @a

    op A @a -> op B @b
}
"""

SRC_PLAIN = SRC_SHAPED.replace("    shape    insert_proxy\n", "")


def _registries() -> tuple[Registries, list]:
    shaper_calls: list = []
    reg = Registries()
    reg.opcodes.update({"A": OP_A, "B": OP_B})
    reg.block_types["MB"] = type("MB", (), {})

    @reg.register_builder("build_shaped")
    async def _build(m):  # noqa: ARG001
        return Block("mb", [], None)

    @reg.register_shaper("insert_proxy")
    def insert_proxy(produced, match):
        # Post-merge evidence: both consumed blocks already absorbed.
        assert match.a in produced.children_blocks
        assert match.b in produced.children_blocks
        proxy = EmptyBlock()
        match.a.insert_between([match.b], proxy)
        produced.children_blocks.append(proxy)
        proxy.parent = produced
        shaper_calls.append((produced, match))

    return reg, shaper_calls


def _topo_sig(block: Block):
    """Structural signature over btype/next_blocks/children_blocks. Children
    are order-insensitive: merge_blocks iterates a set(), so absorption order
    is not part of the contract."""
    return (
        block.btype,
        tuple(_topo_sig(nb) for nb in block.next_blocks),
        tuple(sorted(_topo_sig(cb) for cb in block.children_blocks)),
    )


# ==========================================================================
# Loader / load-time validation
# ==========================================================================


def test_loader_rejects_non_string_shape():
    reg, _ = _registries()
    art_dict = emit_ir(compile_text(SRC_SHAPED, "test.mch", reg), ["test.mch"])
    art_dict["matchers"][0]["shape"] = 42
    with pytest.raises(IrLoadError, match="shape must be a string"):
        load_artifact(art_dict)


def test_build_matchers_hard_fails_on_missing_shaper():
    reg, _ = _registries()
    art = load_artifact(emit_ir(compile_text(SRC_SHAPED, "test.mch", reg), ["test.mch"]))

    bare = Registries()
    bare.opcodes.update(reg.opcodes)
    bare.block_types.update(reg.block_types)
    bare.builders.update(reg.builders)
    # No shapers registered.
    with pytest.raises(IrLoadError, match="shaper 'insert_proxy' is not registered"):
        build_matchers(art, bare)


def test_shape_key_round_trips_through_loader():
    reg, _ = _registries()
    art = load_artifact(emit_ir(compile_text(SRC_SHAPED, "test.mch", reg), ["test.mch"]))
    assert art.matchers[0]["shape"] == "insert_proxy"


# ==========================================================================
# Engine behavior + parity with the compiled path
# ==========================================================================


@pytest.mark.asyncio
async def test_ir_engine_invokes_shaper_post_merge():
    reg, calls = _registries()
    compiled = compile_text(SRC_SHAPED, "test.mch", reg)
    matcher = build_matchers(load_artifact(emit_ir(compiled, ["test.mch"])), reg)[0]
    a, b = _ab_tree()

    result = await matcher.try_build(a)
    assert result is not None and len(result) == 1
    produced = result[0]

    assert len(calls) == 1
    assert calls[0][0] is produced
    proxy = a.next_blocks[0]
    assert isinstance(proxy, EmptyBlock)
    assert proxy.next_blocks == [b]
    assert proxy in produced.children_blocks


@pytest.mark.asyncio
async def test_shaped_topology_places_proxy_in_produced_region():
    reg, _ = _registries()
    compiled = compile_text(SRC_SHAPED, "test.mch", reg)
    ir_matcher = build_matchers(load_artifact(emit_ir(compiled, ["test.mch"])), reg)[0]

    a, _b = _ab_tree()
    ir_result = await ir_matcher.try_build(a)

    assert ir_result
    # The shaper's proxy shows up in the produced block's region.
    assert any(isinstance(cb, EmptyBlock) for cb in ir_result[0].children_blocks)


@pytest.mark.asyncio
async def test_matcher_without_shape_leaves_tree_unaffected():
    reg, calls = _registries()
    compiled = compile_text(SRC_PLAIN, "test.mch", reg)
    art = emit_ir(compiled, ["test.mch"])
    assert "shape" not in art["matchers"][0]
    ir_matcher = build_matchers(load_artifact(art), reg)[0]

    a, b = _ab_tree()
    ir_result = await ir_matcher.try_build(a)

    assert ir_result
    assert calls == []
    assert a.next_blocks == [b]
