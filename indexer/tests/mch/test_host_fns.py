"""Host-function escape hatch (Registries.fns) + well-known build env names.

Covers: fn call from build expressions (async, evaluated args), null
passthrough to fns (unlike builtins/lookups), the injected `consumed`/`anchor`
env names (content + resolver exemption from R010), unknown-callee rejection,
and register_fn duplicate protection.

Synthetic blocks carry fake event nodes (lt/utime bookkeeping) so the
declarative build path's merge_blocks works.
"""
from __future__ import annotations

from types import SimpleNamespace

import pytest

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block
from indexer.events.mch.compile import compile_text
from indexer.events.mch.diagnostics import CompileError
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.registry import Registries
from indexer.events.mch.resolver import resolve

RAW_SRC = "0:" + "11" * 32
RAW_DST = "0:" + "22" * 32

OP_FOO = 0xF00
OP_BAR = 0xBA7


class _FakeTx:
    def __init__(self, lt: int):
        self.lt = lt
        self.now = lt
        self.orig_status = "active"
        self.end_status = "active"


class _FakeMsg:
    def __init__(self, opcode: int, lt: int):
        self.opcode = opcode
        self.source = RAW_SRC
        self.destination = RAW_DST
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


def _link(parent: Block, child: Block) -> None:
    parent.next_blocks.append(child)
    child.previous_block = parent


def _registries() -> Registries:
    reg = Registries()
    reg.opcodes.update({"Foo": OP_FOO, "Bar": OP_BAR})
    reg.block_types["FB"] = object
    return reg


def _compile(src: str, reg: Registries):
    return compile_text(src, "test.mch", reg)[0].matcher


_FN_SRC = """
matcher fn_host {
    produces FB
    entry    @x

    op Foo @x -> maybe op Bar @y

    let r = summarize(consumed, anchor, y)
    reject when r == null
    out {
        n:      r.n
        anchor_is_x: r.anchor_ok
        y_null: r.y_null
    }
}
"""


@pytest.mark.asyncio
async def test_fn_receives_consumed_anchor_and_null_args():
    seen: dict = {}

    async def summarize(consumed, anchor, y):
        seen["consumed"] = consumed
        seen["anchor"] = anchor
        seen["y"] = y
        return SimpleNamespace(n=len(consumed), anchor_ok=anchor is consumed[0],
                               y_null=y is None)

    reg = _registries()
    reg.register_fn("summarize")(summarize)
    matcher = _compile(_FN_SRC, reg)

    # With the optional Bar present: consumed = [anchor, bar].
    anchor = _FakeCC(OP_FOO, lt=10)
    bar = _FakeCC(OP_BAR, lt=20)
    _link(anchor, bar)
    (produced,) = await matcher.try_build(anchor)
    assert produced.data == {"n": 2, "anchor_is_x": True, "y_null": False}
    assert seen["anchor"] is anchor
    assert seen["consumed"][0] is anchor and seen["consumed"][1] is bar

    # Optional absent: the fn still receives null (no builtin-style
    # short-circuit) and consumed shrinks to the anchor alone.
    anchor2 = _FakeCC(OP_FOO, lt=10)
    (produced2,) = await matcher.try_build(anchor2)
    assert produced2.data == {"n": 1, "anchor_is_x": True, "y_null": True}
    assert seen["y"] is None


@pytest.mark.asyncio
async def test_fn_returning_null_supports_reject():
    async def summarize(consumed, anchor, y):
        return None

    reg = _registries()
    reg.register_fn("summarize")(summarize)
    matcher = _compile(_FN_SRC, reg)
    anchor = _FakeCC(OP_FOO, lt=10)
    assert await matcher.try_build(anchor) == []
    assert anchor.parent is None


@pytest.mark.asyncio
async def test_fn_exception_rejects_match(caplog):
    async def summarize(consumed, anchor, y):
        raise RuntimeError("boom")

    reg = _registries()
    reg.register_fn("summarize")(summarize)
    matcher = _compile(_FN_SRC, reg)
    anchor = _FakeCC(OP_FOO, lt=10)
    with caplog.at_level("WARNING", logger="indexer.events.mch.compiler"):
        assert await matcher.try_build(anchor) == []
    assert any("rejecting match" in r.message for r in caplog.records)


@pytest.mark.asyncio
async def test_unknown_callee_rejects_match(caplog):
    src = """
    matcher fn_missing {
        produces FB
        entry    @x

        op Foo @x
        out { v: nosuch(x) }
    }
    """
    matcher = _compile(src, _registries())
    anchor = _FakeCC(OP_FOO, lt=10)
    with caplog.at_level("WARNING", logger="indexer.events.mch.compiler"):
        assert await matcher.try_build(anchor) == []
    assert any("unknown builtin or host fn" in r.message for r in caplog.records)


def test_register_fn_rejects_duplicates():
    reg = Registries()

    async def f():
        return 1

    reg.register_fn("f")(f)
    with pytest.raises(KeyError):
        reg.register_fn("f")(f)


def _codes(src: str) -> list[str]:
    with pytest.raises(CompileError) as exc:
        resolve(parse(list(tokenize(src, "t.mch")), "t.mch"), _registries())
    return [d.code for d in exc.value.bag.items]


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
