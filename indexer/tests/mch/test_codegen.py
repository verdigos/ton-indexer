from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

from indexer.events.mch.codegen import emit
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.registry import Registries
from indexer.events.mch.resolver import resolve


def _resolve(src: str, reg: Registries | None = None):
    toks = list(tokenize(src, "test.mch"))
    f = parse(toks, "test.mch")
    return resolve(f, reg or Registries())


def test_emit_jetton_transfer_match_file(tmp_path: Path):
    src = """
    opcode JettonTransfer         = 0x0f8a7ea5
    opcode JettonInternalTransfer = 0x178d4519
    opcode JettonNotify           = 0x7362d09c

    matcher jetton_transfer {
        produces JettonTransferBlock
        build    build_jetton_transfer
        entry    @transfer

        op JettonTransfer @transfer
          -> op JettonInternalTransfer @internal
          -> maybe op JettonNotify @notify
    }
    """
    reg = Registries()
    reg.block_types["JettonTransferBlock"] = type("JettonTransferBlock", (), {})

    @reg.register_builder("build_jetton_transfer")
    async def _b(m):  # noqa: ARG001
        return None

    rf = _resolve(src, reg)
    written = emit(rf, tmp_path)

    assert len(written) == 1
    p = written[0]
    assert p.name == "jetton_transfer_match.py"
    content = p.read_text(encoding="utf-8")
    assert "@dataclass" in content
    assert "class JettonTransferMatch:" in content
    assert "transfer:" in content
    assert "internal:" in content
    assert "notify:" in content
    assert "| None" in content  # `notify` is optional


def test_emit_idempotent(tmp_path: Path):
    src = """
    opcode Foo = 0x1
    matcher m {
        produces FooBlock
        build    build_m
        entry    @t

        op Foo @t
    }
    """
    reg = Registries()
    reg.block_types["FooBlock"] = type("FooBlock", (), {})

    @reg.register_builder("build_m")
    async def _b(m):  # noqa: ARG001
        return None

    rf = _resolve(src, reg)
    first = emit(rf, tmp_path)[0]
    first_content = first.read_text(encoding="utf-8")
    first_mtime = first.stat().st_mtime_ns

    # Re-emit; content identical, file should not be rewritten.
    second = emit(rf, tmp_path)[0]
    assert second.read_text(encoding="utf-8") == first_content
    assert second.stat().st_mtime_ns == first_mtime


def test_emitted_file_importable(tmp_path: Path):
    src = """
    opcode Foo = 0x1
    matcher widget {
        produces FooBlock
        build    build_widget
        entry    @t

        op Foo @t
    }
    """
    reg = Registries()
    reg.block_types["FooBlock"] = type("FooBlock", (), {})

    @reg.register_builder("build_widget")
    async def _b(m):  # noqa: ARG001
        return None

    rf = _resolve(src, reg)
    written = emit(rf, tmp_path)
    p = written[0]

    spec = importlib.util.spec_from_file_location("widget_match", p)
    module = importlib.util.module_from_spec(spec)  # type: ignore[arg-type]
    sys.modules["widget_match"] = module
    spec.loader.exec_module(module)  # type: ignore[union-attr]

    cls = getattr(module, "WidgetMatch")
    assert hasattr(cls, "__dataclass_fields__")
    assert "t" in cls.__dataclass_fields__
