"""IR expression-evaluator conformance (mch_ir/expr.py).

Mirrors tests/mch/test_eval.py case-for-case: the same source expressions are
parsed through the real frontend and ENCODED TO IR JSON via the emitter, then
evaluated by the IR evaluator — so any semantic drift from eval.py or any
encoding drift from ir_emit.py fails here. (Test code is exempt from the
package's no-frontend-imports boundary.)
"""
from __future__ import annotations

from types import SimpleNamespace

import pytest

from indexer.events.blocks.core import Block
from indexer.events.blocks.utils import AccountId, Amount, Asset
from indexer.events.mch import ast_
from indexer.events.mch.ir_emit import _emit_expr
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.registry import Registries
from indexer.events.mch_ir.expr import EvalError, Evaluator

RAW = "0:" + "00" * 32
RAW2 = "0:" + "aa" * 32


def _expr_json(src: str) -> dict:
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
    return _emit_expr(let.value)


async def _eval(src: str, env: dict | None = None, reg: Registries | None = None,
                bodies: dict | None = None):
    ev = Evaluator(env or {}, reg or Registries(), bodies)
    return await ev.eval(_expr_json(src))


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
    assert await _eval("true or false and false") is True
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
    assert await _eval("x ?? 1 if false else 2", env={"x": 9}) == 2
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


@pytest.mark.asyncio
async def test_unbound_name_is_error():
    with pytest.raises(EvalError):
        await _eval("ghost")


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
# Host fns (registries.fns escape hatch)
# ==========================================================================

@pytest.mark.asyncio
async def test_host_fn_null_args_pass_through():
    seen: list = []
    reg = Registries()

    async def residue(a, b):
        seen.append((a, b))
        return "ok"

    reg.fns["residue"] = residue
    assert await _eval("residue(x, 2)", env={"x": None}, reg=reg) == "ok"
    assert seen == [(None, 2)]


@pytest.mark.asyncio
async def test_host_fn_exception_is_eval_error():
    reg = Registries()

    async def boom():
        raise RuntimeError("nope")

    reg.fns["boom"] = boom
    with pytest.raises(EvalError):
        await _eval("boom()", reg=reg)


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
# Sync where_expr evaluation (dotfield against block.data)
# ==========================================================================

def _where_json(src: str) -> dict:
    """Parse an inline `where (expr)` clause through the frontend and encode it."""
    file_src = (
        "opcode Foo = 0x1\nopcode Bar = 0x2\n"
        "matcher m {\n  produces FB\n  build b\n  entry @x\n\n"
        f"  op Foo @x -> op Bar @y where ({src})\n"
        "}"
    )
    f = parse(list(tokenize(file_src, "t.mch")), "t.mch")
    seq = f.matchers[0].pattern
    node = seq.tail[0][1]
    assert node.where_expr is not None
    return _emit_expr(node.where_expr)


def test_where_expr_reads_block_data():
    from indexer.events.mch_ir.expr import eval_where_expr

    b = Block("call_contract", [], {"value": 10, "kind": "swap"})
    assert eval_where_expr(_where_json(".value > 5"), b) is True
    assert eval_where_expr(_where_json(".value > 50"), b) is False
    assert eval_where_expr(_where_json('.kind == "swap"'), b) is True


def test_where_expr_faults_fail_the_test():
    from indexer.events.mch_ir.expr import eval_where_expr

    # Missing data field, null data, and a non-bool result all read as False.
    assert eval_where_expr(_where_json(".missing == 1"), Block("t", [], {"v": 1})) is False
    assert eval_where_expr(_where_json(".value > 5"), Block("t", [], None)) is False
    assert eval_where_expr(_where_json(".value"), Block("t", [], {"value": 3})) is False


# ==========================================================================
# List combinators (spec §9.6) — edge semantics
# ==========================================================================

@pytest.mark.asyncio
async def test_first_last():
    assert await _eval("first(xs)", env={"xs": [1, 2, 3]}) == 1
    assert await _eval("last(xs)", env={"xs": [1, 2, 3]}) == 3
    # Empty list -> null; a null list argument -> null (call short-circuit).
    assert await _eval("first(xs)", env={"xs": []}) is None
    assert await _eval("last(xs)", env={"xs": []}) is None
    assert await _eval("first(xs)", env={"xs": None}) is None


@pytest.mark.asyncio
async def test_len():
    assert await _eval("len(xs)", env={"xs": [1, 2, 3]}) == 3
    assert await _eval("len(xs)", env={"xs": []}) == 0
    assert await _eval("len(xs)", env={"xs": None}) is None  # null propagation


@pytest.mark.asyncio
async def test_sum():
    assert await _eval("sum(xs)", env={"xs": [1, 2, 3]}) == 6
    assert await _eval("sum(xs)", env={"xs": []}) == 0  # empty -> 0
    # Amount elements are coerced to their integer value.
    assert await _eval("sum(xs)", env={"xs": [Amount(10), Amount(5)]}) == 15
    # A non-numeric element is a fault (match rejection).
    with pytest.raises(EvalError):
        await _eval("sum(xs)", env={"xs": [1, "x"]})


@pytest.mark.asyncio
async def test_zip_truncates_to_shorter():
    assert await _eval("zip(xs, ys)", env={"xs": [1, 2, 3], "ys": [10, 20]}) == [[1, 10], [2, 20]]
    assert await _eval("zip(xs, ys)", env={"xs": [], "ys": [1]}) == []


@pytest.mark.asyncio
async def test_map_field_projection():
    dicts = [{"amount": 1}, {"amount": 2}]
    assert await _eval('map(xs, "amount")', env={"xs": dicts}) == [1, 2]
    objs = [SimpleNamespace(k=7), SimpleNamespace(k=8)]
    assert await _eval('map(xs, "k")', env={"xs": objs}) == [7, 8]
    assert await _eval('map(xs, "amount")', env={"xs": []}) == []
    # A missing field on an element is a fault.
    with pytest.raises(EvalError):
        await _eval('map(xs, "missing")', env={"xs": dicts})


@pytest.mark.asyncio
async def test_combinators_composed():
    # sum of a projected field over a list of records.
    dicts = [{"amount": 3}, {"amount": 4}, {"amount": 5}]
    assert await _eval('sum(map(xs, "amount"))', env={"xs": dicts}) == 12
    assert await _eval('len(map(xs, "amount"))', env={"xs": dicts}) == 3


@pytest.mark.asyncio
async def test_combinator_requires_list():
    with pytest.raises(EvalError):
        await _eval("len(x)", env={"x": 5})


# ==========================================================================
# List / record literals (spec §9.6 G4)
# ==========================================================================

@pytest.mark.asyncio
async def test_list_literal():
    assert await _eval("[1, 2, 3]") == [1, 2, 3]
    assert await _eval("[]") == []
    assert await _eval("[a, b]", env={"a": 7, "b": 8}) == [7, 8]
    # trailing comma / newlines tolerated
    assert await _eval("[1, 2,]") == [1, 2]


@pytest.mark.asyncio
async def test_record_literal():
    assert await _eval("{a: 1, b: 2}") == {"a": 1, "b": 2}
    assert await _eval("{}") == {}
    assert await _eval("{x: a, y: a}", env={"a": 5}) == {"x": 5, "y": 5}


@pytest.mark.asyncio
async def test_nested_literals():
    got = await _eval('{legs: [1, 2], meta: {n: 3}}')
    assert got == {"legs": [1, 2], "meta": {"n": 3}}


@pytest.mark.asyncio
async def test_combinators_over_list_literal():
    assert await _eval("sum([1, 2, 3])") == 6
    assert await _eval("len([1, 2, 3])") == 3
    assert await _eval("first([9, 8])") == 9


# ==========================================================================
# msg.exit_code envelope accessor (spec §9.6, G9 rider)
# ==========================================================================

@pytest.mark.asyncio
async def test_exit_code_envelope_accessor():
    # Defined envelope field: the transaction's compute-phase exit code.
    msg = SimpleNamespace(transaction=SimpleNamespace(compute_exit_code=0xABCD))
    assert await _eval("m.exit_code", env={"m": msg}) == 0xABCD
    # Transaction present but no exit code recorded -> null.
    assert await _eval("m.exit_code", env={"m": SimpleNamespace(transaction=SimpleNamespace())}) is None
    # Not a message envelope (no transaction) -> fault (rejection).
    with pytest.raises(EvalError):
        await _eval("m.exit_code", env={"m": SimpleNamespace()})
