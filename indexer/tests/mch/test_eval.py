"""Evaluator unit tests (mch/eval.py): literals, operator semantics, null
propagation, builtins, lookups, and block accessors.

Expressions are parsed through the real frontend (embedded in a `let`) so the
tests exercise the exact AST shapes the parser emits.
"""
from __future__ import annotations

from types import SimpleNamespace

import pytest

from indexer.events.blocks.core import Block
from indexer.events.blocks.utils import AccountId, Amount, Asset
from indexer.events.mch import ast_
from indexer.events.mch.eval import (
    EvalError,
    Evaluator,
    FlowEffect,
    apply_flows,
)
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.registry import Registries

RAW = "0:" + "00" * 32
RAW2 = "0:" + "aa" * 32


def _expr(src: str) -> ast_.Expr:
    file_src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n  produces FB\n  build b\n  entry @x\n\n"
        "  op Foo @x\n"
        f"  let e = {src}\n"
        "}"
    )
    f = parse(list(tokenize(file_src, "t.mch")), "t.mch")
    let = f.matchers[0].build_stmts[-1]
    assert isinstance(let, ast_.LetStmt)
    return let.value


async def _eval(src: str, env: dict | None = None, reg: Registries | None = None,
                bodies: dict | None = None):
    ev = Evaluator(env or {}, reg or Registries(), bodies)
    return await ev.eval(_expr(src))


# ==========================================================================
# Literals and operators
# ==========================================================================

@pytest.mark.asyncio
async def test_literals():
    assert await _eval("42") == 42
    assert await _eval("0xff") == 255
    assert await _eval('"hi"') == "hi"
    assert await _eval("true") is True
    assert await _eval("false") is False
    assert await _eval("null") is None


@pytest.mark.asyncio
async def test_bool_ops_and_precedence():
    # `and` binds tighter than `or`.
    assert await _eval("true or false and false") is True
    # `not` wraps the comparison, not the operand alone.
    assert await _eval("not 1 == 2") is True
    assert await _eval("not true") is False


@pytest.mark.asyncio
async def test_comparisons():
    assert await _eval("2 > 1") is True
    assert await _eval("1 >= 2") is False
    assert await _eval("1 == 1") is True
    assert await _eval("1 != 2") is True
    assert await _eval("-3 < -2") is True


@pytest.mark.asyncio
async def test_comparison_type_mismatch_is_error():
    with pytest.raises(EvalError):
        await _eval('"a" < 1')


@pytest.mark.asyncio
async def test_and_or_require_bools():
    with pytest.raises(EvalError):
        await _eval("1 and true")
    with pytest.raises(EvalError):
        await _eval("x or true", env={"x": None})
    with pytest.raises(EvalError):
        await _eval("not 5")


@pytest.mark.asyncio
async def test_unary_minus():
    assert await _eval("-5") == -5
    assert await _eval("-x", env={"x": None}) is None


# ==========================================================================
# Ternary and coalesce
# ==========================================================================

@pytest.mark.asyncio
async def test_ternary_branches():
    assert await _eval("1 if true else 2") == 1
    assert await _eval("1 if false else 2") == 2


@pytest.mark.asyncio
async def test_ternary_is_lowest_precedence_and_right_assoc():
    # `a ?? 1 if c else 2` == `(a ?? 1) if c else 2`
    assert await _eval("x ?? 1 if false else 2", env={"x": 9}) == 2
    # right-assoc: `1 if false else 2 if true else 3` == 1 if false else (2 if true else 3)
    assert await _eval("1 if false else 2 if true else 3") == 2


@pytest.mark.asyncio
async def test_ternary_untaken_branch_not_evaluated():
    # `ghost` is unbound; evaluating it would raise.
    assert await _eval("1 if true else ghost") == 1


@pytest.mark.asyncio
async def test_ternary_condition_must_be_bool():
    with pytest.raises(EvalError):
        await _eval("1 if 5 else 2")


@pytest.mark.asyncio
async def test_coalesce():
    assert await _eval("null ?? 5") == 5
    assert await _eval("3 ?? 5") == 3
    # right side is lazy
    assert await _eval("3 ?? ghost") == 3


# ==========================================================================
# Null semantics
# ==========================================================================

@pytest.mark.asyncio
async def test_field_access_on_null_propagates():
    assert await _eval("x.a", env={"x": None}) is None
    assert await _eval("x.a.b.c", env={"x": None}) is None


@pytest.mark.asyncio
async def test_null_equality():
    assert await _eval("x == null", env={"x": None}) is True
    assert await _eval("x != null", env={"x": None}) is False
    assert await _eval("y == null", env={"y": 1}) is False
    assert await _eval("y != null", env={"y": 1}) is True
    # null-vs-null through both sides
    assert await _eval("x == y", env={"x": None, "y": None}) is True


@pytest.mark.asyncio
async def test_ordered_comparison_with_null_is_false():
    assert await _eval("x < 5", env={"x": None}) is False
    assert await _eval("x > 5", env={"x": None}) is False
    assert await _eval("5 <= x", env={"x": None}) is False


@pytest.mark.asyncio
async def test_null_equality_never_calls_user_eq():
    # AccountId.__eq__ raises AttributeError when compared against None;
    # the evaluator must answer structurally instead.
    assert await _eval("a == null", env={"a": AccountId(RAW)}) is False
    assert await _eval("a != null", env={"a": AccountId(RAW)}) is True


# ==========================================================================
# Builtins
# ==========================================================================

@pytest.mark.asyncio
async def test_builtin_account_and_amount():
    assert await _eval(f'account("{RAW}")') == AccountId(RAW)
    assert await _eval("amount(7)") == Amount(7)
    # Amount input is unwrapped, not double-wrapped.
    assert await _eval("amount(a)", env={"a": Amount(9)}) == Amount(9)


@pytest.mark.asyncio
async def test_builtin_asset_and_ton_asset():
    a = await _eval(f'asset("{RAW}")')
    assert isinstance(a, Asset) and a.is_ton is False
    assert a.jetton_address == AccountId(RAW)
    t = await _eval("ton_asset()")
    assert isinstance(t, Asset) and t.is_ton is True and t.jetton_address is None


@pytest.mark.asyncio
async def test_builtin_b64():
    assert await _eval("b64(raw)", env={"raw": b"hello"}) == "aGVsbG8="


@pytest.mark.asyncio
async def test_builtin_null_arg_yields_null():
    assert await _eval("account(null)") is None
    assert await _eval("amount(x)", env={"x": None}) is None
    assert await _eval("b64(x)", env={"x": None}) is None


@pytest.mark.asyncio
async def test_builtin_arity_and_unknown_name():
    with pytest.raises(EvalError):
        await _eval("account()")
    with pytest.raises(EvalError):
        await _eval("frobnicate(1)")


@pytest.mark.asyncio
async def test_builtin_internal_failure_is_eval_error():
    with pytest.raises(EvalError):
        await _eval('amount("not-a-number")')


# ==========================================================================
# Lookups
# ==========================================================================

def _reg_with_lookup(calls: list):
    reg = Registries()

    async def wallet(key):
        calls.append(key)
        return SimpleNamespace(owner="OWNER", jetton=RAW2, address=key)

    reg.lookups["wallet"] = wallet
    return reg


@pytest.mark.asyncio
async def test_lookup_returns_record_and_field_access_works():
    calls: list = []
    reg = _reg_with_lookup(calls)
    assert await _eval('lookup wallet("k").owner', reg=reg) == "OWNER"
    assert calls == ["k"]


@pytest.mark.asyncio
async def test_lookup_null_arg_short_circuits():
    calls: list = []
    reg = _reg_with_lookup(calls)
    assert await _eval("lookup wallet(x)", env={"x": None}, reg=reg) is None
    assert calls == []


@pytest.mark.asyncio
async def test_lookup_unknown_kind_is_error():
    with pytest.raises(EvalError):
        await _eval('lookup nope("k")')


# ==========================================================================
# Block accessors
# ==========================================================================

@pytest.mark.asyncio
async def test_block_flag_and_btype_accessors():
    b = Block("jetton_transfer", [], None)
    b.failed = True
    assert await _eval("x.failed", env={"x": b}) is True
    assert await _eval("x.broken", env={"x": b}) is False
    assert await _eval("x.btype", env={"x": b}) == "jetton_transfer"


@pytest.mark.asyncio
async def test_block_unknown_accessor_is_error():
    b = Block("t", [], None)
    with pytest.raises(EvalError):
        await _eval("x.zzz", env={"x": b})


@pytest.mark.asyncio
async def test_body_before_parse_is_error():
    b = Block("t", [], None)
    with pytest.raises(EvalError):
        await _eval("x.body", env={"x": b})


@pytest.mark.asyncio
async def test_body_after_parse_reads_parsed_attrs():
    b = Block("t", [], None)
    bodies = {id(b): SimpleNamespace(amount=5)}
    assert await _eval("x.body.amount", env={"x": b}, bodies=bodies) == 5


@pytest.mark.asyncio
async def test_list_capture_body_is_parallel():
    b1, b2 = Block("t", [], None), Block("t", [], None)
    bodies = {id(b1): SimpleNamespace(v=1), id(b2): None}
    xs = [b1, None, b2]
    assert await _eval("xs.body", env={"xs": xs}, bodies=bodies) == [
        bodies[id(b1)], None, None,
    ]


@pytest.mark.asyncio
async def test_list_capture_other_field_is_error():
    with pytest.raises(EvalError):
        await _eval("xs.failed", env={"xs": [Block("t", [], None)]})


@pytest.mark.asyncio
async def test_getattr_on_plain_object_and_missing_attr():
    o = SimpleNamespace(owner="O")
    assert await _eval("x.owner", env={"x": o}) == "O"
    with pytest.raises(EvalError):
        await _eval("x.missing", env={"x": o})


@pytest.mark.asyncio
async def test_field_ref_outside_where_is_error():
    with pytest.raises(EvalError):
        await _eval(".amount")


# ==========================================================================
# apply_flows
# ==========================================================================

def test_apply_flows_records_effects_and_skips_null_legs():
    b = Block("t", [], None)
    owner, master = AccountId(RAW), AccountId(RAW2)
    apply_flows(b, [
        FlowEffect("ton", (owner, Amount(5))),
        FlowEffect("fees", (owner, 2)),
        FlowEffect("jetton", (owner, master, 10)),
        FlowEffect("ton", (None, 100)),  # null leg: skipped
    ])
    flow = b.value_flow.flow[owner]
    assert flow.ton == 5
    assert flow.fees == 2
    assert flow.jettons[master] == 10
