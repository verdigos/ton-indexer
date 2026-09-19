"""`shape` directive grammar, registry, diagnostics, and IR emission."""
from __future__ import annotations

import pytest

from indexer.events.blocks.core import EmptyBlock
from indexer.events.mch.compile import compile_text
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
        {shape_line}

        op A @a -> op B @b
        out {{ v: 1 }}
    }}
    """


def _registries() -> Registries:
    reg = Registries()
    reg.opcodes.update({"A": OP_A, "B": OP_B})

    @reg.register_shaper("insert_proxy")
    def insert_proxy(produced, match):  # pragma: no cover - registration fixture
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
    art = emit_ir(compiled)
    assert art["matchers"][0]["shape"] == "insert_proxy"


def test_emit_ir_omits_shape_when_absent():
    reg = _registries()
    compiled = compile_text(_src(), "test.mch", reg)
    art = emit_ir(compiled)
    assert "shape" not in art["matchers"][0]
