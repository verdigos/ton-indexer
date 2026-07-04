"""M4 build-language frontend: lexer, parser, and resolver coverage.

The compiler ignores build statements entirely in v0 (proof-of-concept branch);
these tests exercise tokenizing, AST construction, and the one new resolver
diagnostic (R010). See docs/mch-spec.md §9 for the normative build language.
"""
from __future__ import annotations

import pytest

from indexer.events.mch import ast_
from indexer.events.mch.diagnostics import CompileError
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.registry import Registries
from indexer.events.mch.resolver import resolve
from indexer.events.mch.tokens import TokenKind


# ==========================================================================
# Helpers
# ==========================================================================

def _kinds(text: str) -> list[TokenKind]:
    return [t.kind for t in tokenize(text, "f.mch") if t.kind is not TokenKind.EOF]


def _parse(src: str) -> ast_.File:
    return parse(list(tokenize(src, "test.mch")), "test.mch")


def _matcher(body: str) -> ast_.MatcherDecl:
    """Wrap a matcher body (pattern + build stmts) in a full file and return the
    single MatcherDecl. Opcode Foo and block type FB are predeclared."""
    src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n"
        "  produces FB\n"
        "  build b\n"
        "  entry @x\n\n"
        + body
        + "\n}"
    )
    return _parse(src).matchers[0]


def _expr(src: str) -> ast_.Expr:
    """Parse a bare expression by embedding it in a `let`."""
    m = _matcher("op Foo @x\nlet e = " + src)
    let = m.build_stmts[-1]
    assert isinstance(let, ast_.LetStmt)
    return let.value


def _fb_registries() -> Registries:
    reg = Registries()
    reg.block_types["FB"] = object
    return reg


def _resolve(src: str):
    return resolve(_parse(src), _fb_registries())


# ==========================================================================
# Lexer
# ==========================================================================

def test_lex_comparison_and_expr_operators():
    assert _kinds("== != <= >= < > ?? - , :") == [
        TokenKind.EQEQ, TokenKind.NEQ, TokenKind.LE, TokenKind.GE,
        TokenKind.LT, TokenKind.GT, TokenKind.COALESCE, TokenKind.MINUS,
        TokenKind.COMMA, TokenKind.COLON,
    ]


def test_lex_new_keywords_promoted():
    assert _kinds("not and or let parse as out reject failed broken when lookup null") == [
        TokenKind.KW_NOT, TokenKind.KW_AND, TokenKind.KW_OR, TokenKind.KW_LET,
        TokenKind.KW_PARSE, TokenKind.KW_AS, TokenKind.KW_OUT, TokenKind.KW_REJECT,
        TokenKind.KW_FAILED, TokenKind.KW_BROKEN, TokenKind.KW_WHEN,
        TokenKind.KW_LOOKUP, TokenKind.KW_NULL,
    ]


def test_lex_arrow_vs_minus_disambiguation():
    # `->` and `<-` stay edges; bare `-` is MINUS, `<=`/`<` are comparisons.
    assert _kinds("-> - <- <= <") == [
        TokenKind.ARROW_R, TokenKind.MINUS, TokenKind.ARROW_L,
        TokenKind.LE, TokenKind.LT,
    ]


def test_lex_bare_bang_is_error():
    from indexer.events.mch.lexer import LexerError
    with pytest.raises(LexerError) as exc:
        list(tokenize("a ! b", "f.mch"))
    assert exc.value.code == "L002_UNKNOWN_CHAR"


# ==========================================================================
# Parser — build statements
# ==========================================================================

def test_parse_stmt_roundtrips():
    m = _matcher("op Foo @x\nparse x as JettonTransfer")
    assert len(m.build_stmts) == 1
    ps = m.build_stmts[0]
    assert isinstance(ps, ast_.ParseStmt)
    assert ps.capture == "x"
    assert ps.msg_type == "JettonTransfer"


def test_let_with_lookup():
    m = _matcher("op Foo @x\nlet wallet = lookup jetton_wallet(x.msg.destination)")
    let = m.build_stmts[0]
    assert isinstance(let, ast_.LetStmt)
    assert let.name == "wallet"
    assert isinstance(let.value, ast_.LookupExpr)
    assert let.value.kind == "jetton_wallet"
    assert len(let.value.args) == 1
    assert isinstance(let.value.args[0], ast_.FieldAccess)


def test_reject_failed_broken_when():
    m = _matcher(
        "op Foo @x\n"
        "reject when x == null\n"
        "failed when x.failed\n"
        "broken when x.broken"
    )
    kinds = [type(s) for s in m.build_stmts]
    assert kinds == [ast_.RejectStmt, ast_.FailedStmt, ast_.BrokenStmt]
    assert isinstance(m.build_stmts[0].condition, ast_.BinaryOp)


def test_out_block_newline_and_comma_separators():
    m = _matcher(
        "op Foo @x\n"
        "out {\n"
        "  sender: x.msg.source\n"
        "  amount: x.body.amount, query_id: x.body.query_id\n"
        "}"
    )
    out = m.build_stmts[0]
    assert isinstance(out, ast_.OutStmt)
    assert [f.name for f in out.fields] == ["sender", "amount", "query_id"]


def test_full_matcher_combines_pattern_and_build_stmts():
    m = _matcher(
        "op Foo @x -> maybe op Foo @y\n"
        "parse x as JettonTransfer\n"
        "let amt = x.body.amount ?? 0\n"
        "reject when amt == null\n"
        "failed when x.failed or y.failed\n"
        "out { amount: amt; sender: x.msg.source }"
    )
    # Pattern still parses into the usual tree.
    assert isinstance(m.pattern, ast_.Sequence)
    # All five build statements captured in source order.
    assert [type(s).__name__ for s in m.build_stmts] == [
        "ParseStmt", "LetStmt", "RejectStmt", "FailedStmt", "OutStmt",
    ]


# ==========================================================================
# Parser — expression precedence
# ==========================================================================

def test_or_binds_looser_than_and():
    # a or b and c  ==  a or (b and c)
    e = _expr("a or b and c")
    assert isinstance(e, ast_.BinaryOp) and e.op == "or"
    assert isinstance(e.left, ast_.NameRef) and e.left.name == "a"
    assert isinstance(e.right, ast_.BinaryOp) and e.right.op == "and"


def test_not_wraps_comparison():
    # not a == b  ==  not (a == b)
    e = _expr("not a == b")
    assert isinstance(e, ast_.UnaryOp) and e.op == "not"
    assert isinstance(e.operand, ast_.BinaryOp) and e.operand.op == "=="


def test_coalesce_binds_tighter_than_comparison():
    # a ?? b == c  ==  (a ?? b) == c
    e = _expr("a ?? b == c")
    assert isinstance(e, ast_.BinaryOp) and e.op == "=="
    assert isinstance(e.left, ast_.BinaryOp) and e.left.op == "??"
    assert isinstance(e.right, ast_.NameRef) and e.right.name == "c"


def test_unary_minus_binds_tighter_than_coalesce():
    # a ?? -b  ==  a ?? (-b)
    e = _expr("a ?? -b")
    assert isinstance(e, ast_.BinaryOp) and e.op == "??"
    assert isinstance(e.right, ast_.UnaryOp) and e.right.op == "-"


def test_postfix_field_access_chain_and_call():
    e = _expr("jetton(wallet.jetton)")
    assert isinstance(e, ast_.Call)
    assert isinstance(e.callee, ast_.NameRef) and e.callee.name == "jetton"
    assert isinstance(e.args[0], ast_.FieldAccess)
    assert e.args[0].field == "jetton"
    assert isinstance(e.args[0].target, ast_.NameRef)


def test_literals_parse():
    assert isinstance(_expr("0xff"), ast_.IntLit) and _expr("0xff").value == 255
    assert isinstance(_expr("42"), ast_.IntLit) and _expr("42").value == 42
    assert isinstance(_expr("true"), ast_.BoolLit) and _expr("true").value is True
    assert isinstance(_expr("null"), ast_.NullLit)
    s = _expr('"hi"')
    assert isinstance(s, ast_.StrLit) and s.value == "hi"


def test_where_expr_on_node():
    m = _matcher("op Foo @x where (.amount == 5)")
    node = m.pattern
    assert isinstance(node, ast_.Node)
    assert node.where_predicate is None
    assert isinstance(node.where_expr, ast_.BinaryOp) and node.where_expr.op == "=="
    # leading-dot field is a FieldRef primary
    assert isinstance(node.where_expr.left, ast_.FieldRef)
    assert node.where_expr.left.field == "amount"


def test_where_named_predicate_still_parses():
    src = (
        "opcode Foo = 0x1\npredicate p\n"
        "rule r = op Foo @x where p"
    )
    node = _parse(src).rules[0].pattern
    assert isinstance(node, ast_.Node)
    assert node.where_predicate == "p"
    assert node.where_expr is None


# ==========================================================================
# Resolver
# ==========================================================================

def test_resolver_carries_build_stmts():
    rf = _resolve(
        "opcode Foo = 0x1\n"
        "matcher m {\n  produces FB\n  build b\n  entry @x\n\n"
        "  op Foo @x\n  let a = x.body\n}"
    )
    assert len(rf.matchers[0].build_stmts) == 1
    assert isinstance(rf.matchers[0].build_stmts[0], ast_.LetStmt)


def test_resolver_known_capture_ref_passes():
    # x is a capture; jetton(...) callee is a builtin (unvalidated).
    rf = _resolve(
        "opcode Foo = 0x1\n"
        "matcher m {\n  produces FB\n  build b\n  entry @x\n\n"
        "  op Foo @x\n"
        "  let amt = x.body.amount\n"
        "  out { amount: amt, asset: jetton(x) }\n}"
    )
    assert rf.matchers[0].build_stmts  # resolved without error


def test_resolver_unknown_ref_emits_r010():
    with pytest.raises(CompileError) as exc:
        _resolve(
            "opcode Foo = 0x1\n"
            "matcher m {\n  produces FB\n  build b\n  entry @x\n\n"
            "  op Foo @x\n  reject when ghost == 1\n}"
        )
    codes = [d.code for d in exc.value.bag.items]
    assert "R010_UNRESOLVED_REFERENCE" in codes


def test_resolver_let_name_visible_to_later_stmt():
    rf = _resolve(
        "opcode Foo = 0x1\n"
        "matcher m {\n  produces FB\n  build b\n  entry @x\n\n"
        "  op Foo @x\n"
        "  let a = x.body\n"
        "  out { v: a }\n}"
    )
    assert len(rf.matchers[0].build_stmts) == 2


def test_resolver_forward_let_reference_is_unresolved():
    # `b` is declared after it is used -> unresolved at point of use.
    with pytest.raises(CompileError) as exc:
        _resolve(
            "opcode Foo = 0x1\n"
            "matcher m {\n  produces FB\n  build b\n  entry @x\n\n"
            "  op Foo @x\n"
            "  let a = b\n"
            "  let b = x.body\n}"
        )
    assert "R010_UNRESOLVED_REFERENCE" in [d.code for d in exc.value.bag.items]
