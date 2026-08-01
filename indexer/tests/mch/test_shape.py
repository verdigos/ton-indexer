"""R06 `shape` directive — frontend: grammar, registry, diagnostics, IR emission
(spec §8.7, docs/mch-ir-poc.md matcher record).

T29 stage 1 retired the compiled runtime, so the shaper's runtime behavior
(post-merge invocation, proxy insertion, topology, declarative-path capture env)
now runs against the v2 IR engine in tests/mch_ir/test_shape.py and
tests/mch_ir/test_compiled_ports.py.
"""
from __future__ import annotations

import pytest

from indexer.events.blocks.core import Block, EmptyBlock
from indexer.events.mch.compile import compile_text
from indexer.events.mch.diagnostics import CompileError
from indexer.events.mch.ir_emit import emit_ir
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.registry import Registries

OP_A = 0x1
OP_B = 0x2


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


def _registries() -> Registries:
    reg = Registries()
    reg.opcodes.update({"A": OP_A, "B": OP_B})
    reg.block_types["MB"] = type("MB", (), {})

    @reg.register_builder("build_shaped")
    async def _build(m):  # noqa: ARG001
        return Block("mb", [], None)

    @reg.register_shaper("insert_proxy")
    def insert_proxy(produced, match):  # pragma: no cover - invoked only at runtime
        proxy = EmptyBlock()
        match.a.insert_between([match.b], proxy)
        produced.children_blocks.append(proxy)
        proxy.parent = produced

    return reg


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


@pytest.mark.parametrize("lines", [
    "produces MB",
    "build build_shaped",
    "entry @a",
    "include_excess true\n        include_excess false",
    "include_bounces true\n        include_bounces false",
])
def test_duplicate_matcher_directive_is_parse_error(lines):
    with pytest.raises(CompileError) as ei:
        _parse(_src(lines))
    assert any(d.code == "P011_DUPLICATE_DIRECTIVE" for d in ei.value.bag.items)


def test_unknown_shaper_is_compile_error():
    reg = _registries()
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
# IR emission
# ==========================================================================


def test_emit_ir_carries_shape_key():
    reg = _registries()
    compiled = compile_text(_src("shape    insert_proxy"), "test.mch", reg)
    art = emit_ir(compiled, ["test.mch"])
    assert art["matchers"][0]["shape"] == "insert_proxy"


def test_emit_ir_omits_shape_when_absent():
    reg = _registries()
    compiled = compile_text(_src(), "test.mch", reg)
    art = emit_ir(compiled, ["test.mch"])
    assert "shape" not in art["matchers"][0]
