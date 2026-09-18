"""Parser, resolver, IR emit, and where-rejection for find/stable_sort_by/at."""
from __future__ import annotations

import pytest

from indexer.events.mch import ast_
from indexer.events.mch.compile import compile_text
from indexer.events.mch.diagnostics import CompileError, R024_NESTED_COMPREHENSION, R033_FN_BODY_COMPREHENSION
from indexer.events.mch.expr_ir import emit_expr
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.registry import Registries


def _parse_let(src: str) -> ast_.Expr:
    file_src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n  produces FB\n\n  op Foo @x\n"
        f"  let e = {src}\n  out {{ v: 1 }}\n}}"
    )
    f = parse(list(tokenize(file_src, "t.mch")), "t.mch")
    let = f.matchers[0].build_stmts[0]
    assert isinstance(let, ast_.LetStmt)
    return let.value


@pytest.mark.parametrize("src,kind,k", [
    ("find(xs as s => s > 0)", "find", "find"),
    ("stable_sort_by(xs as x => x)", "stable_sort_by", "sortc"),
    ("map(xs as x => x)", "map", "mapc"),
])
def test_comprehension_parse_and_ir_kind(src, kind, k):
    e = _parse_let(src)
    assert isinstance(e, ast_.Comprehension)
    assert e.kind == kind
    assert e.var in ("s", "x")
    ir = emit_expr(e)
    assert ir["k"] == k
    assert ir["xs"] == {"k": "name", "id": "xs"}
    assert ir["var"] == e.var


def test_at_parses_as_call_not_comprehension():
    e = _parse_let("at(xs, 0)")
    assert isinstance(e, ast_.Call)
    assert e.callee.name == "at"
    assert len(e.args) == 2
    ir = emit_expr(e)
    assert ir == {
        "k": "call",
        "fn": "at",
        "args": [{"k": "name", "id": "xs"}, {"k": "int", "v": 0}],
    }


def test_find_not_confused_with_plain_call():
    e = _parse_let("find(xs)")
    assert isinstance(e, ast_.Call)
    assert e.callee.name == "find"


def _codes(src: str) -> list[str]:
    try:
        compile_text(src, "t.mch", Registries())
    except CompileError as ex:
        return [d.code for d in ex.bag.items]
    return []


def test_nested_find_rejected_r024():
    src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n  produces FB\n\n  op Foo @x\n"
        "  let e = find(map(xs as a => a) as b => true)\n"
        "  out { v: 1 }\n}"
    )
    assert R024_NESTED_COMPREHENSION in _codes(src)


def test_nested_sort_rejected_r024():
    src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n  produces FB\n\n  op Foo @x\n"
        "  let e = stable_sort_by(find(xs as a => true) as b => b)\n"
        "  out { v: 1 }\n}"
    )
    assert R024_NESTED_COMPREHENSION in _codes(src)


def test_find_in_where_rejected_c013():
    src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n  produces FB\n\n"
        "  op Foo @x where (find([1] as s => true))\n"
        "  out { v: 1 }\n}"
    )
    assert "C013_WHERE_EXPR_NOT_SYNC" in _codes(src)


def test_sort_in_where_rejected_c013():
    src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n  produces FB\n\n"
        "  op Foo @x where (stable_sort_by([1] as s => s))\n"
        "  out { v: 1 }\n}"
    )
    assert "C013_WHERE_EXPR_NOT_SYNC" in _codes(src)


def test_find_in_fn_body_rejected_r033():
    src = (
        "fn f(xs) = find(xs as x => true)\n"
        "opcode Foo = 0x1\n"
        "matcher m {\n  produces FB\n\n  op Foo @x\n  out { v: 1 }\n}"
    )
    assert R033_FN_BODY_COMPREHENSION in _codes(src)


def test_emit_expr_find_shape():
    e = _parse_let("find(xs as s => s > 2)")
    assert emit_expr(e) == {
        "k": "find",
        "xs": {"k": "name", "id": "xs"},
        "var": "s",
        "body": {
            "k": "bin",
            "op": ">",
            "l": {"k": "name", "id": "s"},
            "r": {"k": "int", "v": 2},
        },
    }
