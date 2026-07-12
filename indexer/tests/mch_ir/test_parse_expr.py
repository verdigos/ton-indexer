"""W7 — `parse <target> as T (| T)*` EXPRESSION-form conformance.

The parse expression evaluates the target to a block, parses its body against
the message types in source order (first success wins), and FAULTS (build
reject) on a null/non-block target or when no type parses — there is no
soft-null variant (spec §0.5(a)). This exercises the async build-expression
evaluator's control flow with fake block/message stand-ins (real BOC parsing is
covered end-to-end by the build-vector + integration gates in W7b/W7c), plus the
loader's structural + where-sync checks and the resolver's R012 message-type
check.
"""
from __future__ import annotations

import pytest

from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse as parse_file
from indexer.events.mch.registry import Registries
from indexer.events.mch_ir.expr import EvalError, Evaluator
from indexer.events.mch_ir.loader import IrLoadError, _check_expr, _check_where_expr_sync


class _FakeMsg:
    """A stand-in message parser: raises on a body it can't parse (mirrors a
    pytoniq parser underflowing), else exposes fields as attributes."""

    def __init__(self, body):
        if body == "bad":
            raise ValueError("cannot parse this body")
        self.body = body
        self.amount = 42


class _OtherMsg:
    def __init__(self, body):
        self.body = body
        self.tag = "other"


class _FakeBlock:
    def __init__(self, body):
        self._body = body

    def get_body(self):
        return self._body


def _reg() -> Registries:
    r = Registries()
    r.message_types["FakeMsg"] = _FakeMsg
    r.message_types["OtherMsg"] = _OtherMsg
    return r


_PARSE = {"k": "parse", "x": {"k": "name", "id": "blk"}, "types": ["FakeMsg"]}


@pytest.mark.asyncio
async def test_parse_success_returns_message():
    ev = Evaluator({"blk": _FakeBlock("ok")}, _reg())
    got = await ev.eval(_PARSE)
    assert isinstance(got, _FakeMsg) and got.amount == 42


@pytest.mark.asyncio
async def test_parse_all_types_fail_faults():
    ev = Evaluator({"blk": _FakeBlock("bad")}, _reg())
    with pytest.raises(EvalError):
        await ev.eval(_PARSE)


@pytest.mark.asyncio
async def test_parse_null_target_faults():
    # No soft-null variant: a null target rejects (spec §0.5(a)).
    ev = Evaluator({"blk": None}, _reg())
    with pytest.raises(EvalError):
        await ev.eval(_PARSE)


@pytest.mark.asyncio
async def test_parse_non_block_target_faults():
    ev = Evaluator({"blk": 7}, _reg())
    with pytest.raises(EvalError):
        await ev.eval(_PARSE)


@pytest.mark.asyncio
async def test_parse_unregistered_type_faults():
    node = {"k": "parse", "x": {"k": "name", "id": "blk"}, "types": ["NopeMsg"]}
    ev = Evaluator({"blk": _FakeBlock("ok")}, _reg())
    with pytest.raises(EvalError):
        await ev.eval(node)


@pytest.mark.asyncio
async def test_parse_type_alternatives_first_success_wins():
    # First alternative faults on this body, second parses it.
    node = {"k": "parse", "x": {"k": "name", "id": "blk"}, "types": ["FakeMsg", "OtherMsg"]}
    ev = Evaluator({"blk": _FakeBlock("bad")}, _reg())
    got = await ev.eval(node)
    assert isinstance(got, _OtherMsg) and got.tag == "other"


@pytest.mark.asyncio
async def test_parse_inside_map_comprehension():
    # The DEX hop-loop enabler: parse each element of a list.
    node = {
        "k": "mapc", "xs": {"k": "name", "id": "xs"}, "var": "s",
        "body": {"k": "parse", "x": {"k": "name", "id": "s"}, "types": ["FakeMsg"]},
    }
    ev = Evaluator({"xs": [_FakeBlock("a"), _FakeBlock("b")]}, _reg())
    got = await ev.eval(node)
    assert len(got) == 2 and all(isinstance(m, _FakeMsg) for m in got)


# --- loader structural + where-sync checks ---

def test_loader_accepts_parse_node():
    _check_expr("t", _PARSE)  # no raise


def test_loader_rejects_parse_missing_types():
    with pytest.raises(IrLoadError):
        _check_expr("t", {"k": "parse", "x": {"k": "name", "id": "b"}, "types": []})


def test_loader_rejects_parse_in_where_expr():
    # A parse expression is build-only; it cannot appear in a sync where_expr.
    with pytest.raises(IrLoadError):
        _check_where_expr_sync("t", _PARSE)


# --- resolver: R012 for an unregistered message type ---

def test_resolver_flags_unregistered_parse_type():
    from indexer.events.mch.diagnostics import CompileError, Severity
    from indexer.events.mch.resolver import resolve

    reg = Registries()
    reg.block_types["FB"] = object
    reg.message_types["FakeMsg"] = _FakeMsg
    src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n produces FB\n entry @x\n op Foo @x\n"
        "  let m = parse x as NopeMsg\n  out { r: m }\n}"
    )
    f = parse_file(list(tokenize(src, "t.mch")), "t.mch")
    with pytest.raises(CompileError) as exc:
        resolve(f, reg)
    codes = [d.code for d in exc.value.bag.items if d.severity is Severity.ERROR]
    assert "R012_UNKNOWN_MESSAGE_TYPE" in codes, codes
