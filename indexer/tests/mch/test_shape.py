"""R06 `shape` directive: grammar, registry, diagnostics, compiled-path
behavior, and IR emission (spec §8.7, docs/mch-ir-poc.md matcher record).

Synthetic blocks carry fake event nodes (same pattern as
tests/mch_ir/test_engine.py) so merge_blocks works on the compiled path.
"""
from __future__ import annotations

from types import SimpleNamespace

import pytest

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block, EmptyBlock
from indexer.events.mch.compile import compile_text
from indexer.events.mch.diagnostics import CompileError
from indexer.events.mch.ir_emit import emit_ir
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.registry import Registries

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


def _src(shape_line: str = "") -> str:
    return f"""
    opcode A = 0x1
    opcode B = 0x2

    matcher shaped {{
        produces MB
        build    build_shaped
        {shape_line}
        entry    @a

        op A @a -> op B @b
    }}
    """


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


# ==========================================================================
# Grammar
# ==========================================================================


def _parse(src: str):
    return parse(list(tokenize(src, "test.mch")), "test.mch")


def test_shape_directive_parses():
    file = _parse(_src("shape    insert_proxy"))
    assert file.matchers[0].shape == "insert_proxy"


def test_shape_absent_is_none():
    file = _parse(_src())
    assert file.matchers[0].shape is None


def test_duplicate_shape_directive_is_parse_error():
    src = _src("shape    insert_proxy\n        shape    insert_proxy")
    with pytest.raises(CompileError) as ei:
        _parse(src)
    assert any(d.code == "P011_DUPLICATE_DIRECTIVE" for d in ei.value.bag.items)


def test_unknown_shaper_is_compile_error():
    reg, _ = _registries()
    with pytest.raises(CompileError) as ei:
        compile_text(_src("shape    no_such_shaper"), "test.mch", reg)
    assert any(d.code == "C011_UNKNOWN_SHAPER" for d in ei.value.bag.items)


# ==========================================================================
# Registry
# ==========================================================================


def test_register_shaper_via_method():
    reg = Registries()

    @reg.register_shaper("s")
    def s(produced, match):  # noqa: ARG001
        return None

    assert reg.shapers["s"] is s


def test_double_shaper_registration_raises():
    reg = Registries()

    @reg.register_shaper("s")
    def first(produced, match):  # noqa: ARG001
        return None

    with pytest.raises(KeyError, match="shaper 's' already registered"):
        @reg.register_shaper("s")
        def second(produced, match):  # noqa: ARG001
            return None


def test_shapers_empty_by_default():
    assert Registries().shapers == {}


# ==========================================================================
# Compiled-path behavior
# ==========================================================================


@pytest.mark.asyncio
async def test_shaper_runs_after_merge_and_inserts_proxy():
    reg, calls = _registries()
    matcher = compile_text(_src("shape    insert_proxy"), "test.mch", reg)[0].matcher
    a, b = _ab_tree()

    result = await matcher.try_build(a)
    assert result is not None and len(result) == 1
    produced = result[0]

    assert len(calls) == 1
    assert calls[0][0] is produced
    # Proxy inserted inside the produced region: a -> proxy -> b.
    assert len(a.next_blocks) == 1
    proxy = a.next_blocks[0]
    assert isinstance(proxy, EmptyBlock)
    assert proxy.next_blocks == [b]
    assert proxy.previous_block is a
    assert proxy in produced.children_blocks
    assert proxy.parent is produced


@pytest.mark.asyncio
async def test_matcher_without_shape_is_unaffected():
    reg, calls = _registries()
    matcher = compile_text(_src(), "test.mch", reg)[0].matcher
    a, b = _ab_tree()

    result = await matcher.try_build(a)
    assert result is not None and len(result) == 1
    assert calls == []
    assert a.next_blocks == [b]
    assert not any(isinstance(nb, EmptyBlock) for nb in a.next_blocks)


@pytest.mark.asyncio
async def test_shaper_runs_on_declarative_build_path():
    reg, calls = _registries()
    src = """
    opcode A = 0x1
    opcode B = 0x2

    matcher shaped_decl {
        produces MB
        shape    insert_proxy
        entry    @a

        op A @a -> op B @b

        out { kind: "x" }
    }
    """
    matcher = compile_text(src, "test.mch", reg)[0].matcher
    a, b = _ab_tree()

    result = await matcher.try_build(a)
    assert result is not None and len(result) == 1
    assert len(calls) == 1
    produced, match = calls[0]
    assert result[0] is produced
    # Declarative path passes the capture env incl. well-known names (§8.7).
    assert match.a is a
    assert match.b is b
    assert match.anchor is a
    assert isinstance(a.next_blocks[0], EmptyBlock)


# ==========================================================================
# IR emission
# ==========================================================================


def test_emit_ir_carries_shape_key():
    reg, _ = _registries()
    compiled = compile_text(_src("shape    insert_proxy"), "test.mch", reg)
    art = emit_ir(compiled, ["test.mch"])
    assert art["matchers"][0]["shape"] == "insert_proxy"


def test_emit_ir_omits_shape_when_absent():
    reg, _ = _registries()
    compiled = compile_text(_src(), "test.mch", reg)
    art = emit_ir(compiled, ["test.mch"])
    assert "shape" not in art["matchers"][0]
