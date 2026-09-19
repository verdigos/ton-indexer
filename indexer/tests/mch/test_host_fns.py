"""Host-function resolver behavior and injected build-environment names.

These tests verify resolver exemptions for `consumed` and `anchor`, along with
duplicate registration diagnostics. Runtime behavior is covered by engine CTest
vector corpora, walker cases, and action goldens.
"""
from __future__ import annotations

import pytest

from indexer.events.mch.diagnostics import CompileError
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.registry import Registries
from indexer.events.mch.resolver import resolve

OP_FOO = 0xF00
OP_BAR = 0xBA7


def _registries() -> Registries:
    reg = Registries()
    reg.opcodes.update({"Foo": OP_FOO, "Bar": OP_BAR})
    return reg


def _codes(src: str) -> list[str]:
    with pytest.raises(CompileError) as exc:
        resolve(parse(list(tokenize(src, "t.mch")), "t.mch"), _registries())
    return [d.code for d in exc.value.bag.items]


def test_register_fn_rejects_duplicates():
    reg = Registries()

    async def f():
        return 1

    reg.register_fn("f")(f)
    with pytest.raises(KeyError):
        reg.register_fn("f")(f)


def test_well_known_names_do_not_r010():
    src = (
        "opcode Foo = 0xF00\n"
        "matcher m {\n  produces FB\n\n  op Foo @x\n"
        "  out { c: consumed, a: anchor }\n}"
    )
    rf = resolve(parse(list(tokenize(src, "t.mch")), "t.mch"), _registries())


def test_let_may_not_shadow_well_known_names():
    src = (
        "opcode Foo = 0xF00\n"
        "matcher m {\n  produces FB\n\n  op Foo @x\n"
        "  let consumed = 1\n  out { v: consumed }\n}"
    )
    assert "R013_DUP_LET" in _codes(src)
