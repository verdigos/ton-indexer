"""Host-function escape hatch (Registries.fns) + well-known build env names —
frontend resolver behavior.

T29 stage 1 retired the compiled runtime, so the runtime side of host functions
(fn call with evaluated/null args, fn-returns-null => reject, fn-raises =>
reject, unknown-callee => reject) now runs against the v2 IR engine in
tests/mch_ir/test_compiled_ports.py. What remains here is the resolver exemption
for the injected `consumed`/`anchor` env names and register_fn duplicate
protection.
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
    reg.block_types["FB"] = object
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
        "matcher m {\n  produces FB\n  entry @x\n\n  op Foo @x\n"
        "  out { c: consumed, a: anchor }\n}"
    )
    rf = resolve(parse(list(tokenize(src, "t.mch")), "t.mch"), _registries())
    assert rf.matchers[0].decl.build is None


def test_let_may_not_shadow_well_known_names():
    src = (
        "opcode Foo = 0xF00\n"
        "matcher m {\n  produces FB\n  entry @x\n\n  op Foo @x\n"
        "  let consumed = 1\n  out { v: consumed }\n}"
    )
    assert "R013_DUP_LET" in _codes(src)
