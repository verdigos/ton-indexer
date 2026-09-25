from __future__ import annotations

from indexer.events.mch.diagnostics import Span
from indexer.events.mch.tokens import Token, TokenKind


def test_token_kinds_exhaustive_set_present():
    expected = {
        # literals
        "IDENT", "HEX", "DEC", "STRING",
        # declaration keywords
        "KW_OPCODE", "KW_PREDICATE", "KW_RULE", "KW_MATCHER",
        # node-head keywords
        "KW_OP", "KW_BTYPE", "KW_PRED", "KW_ANY",
        # modifier / directive keywords
        "KW_MAYBE", "KW_PEEK", "KW_WHERE", "KW_PRODUCES",
        "KW_INCLUDE_EXCESS", "KW_INCLUDE_BOUNCES",
        # boolean literals
        "KW_TRUE", "KW_FALSE",
        # punctuation
        "ARROW_R", "ARROW_L", "PIPE", "AT", "DOLLAR",
        "LBRACE", "RBRACE", "LPAREN", "RPAREN", "EQ", "DOT", "SEMI",
        # control
        "NEWLINE", "EOF",
    }
    actual = {k.name for k in TokenKind}
    assert expected.issubset(actual)


def test_token_value_object():
    span = Span("f.mch", 1, 1, 1, 4, 0, 3)
    tok = Token(kind=TokenKind.IDENT, text="foo", span=span)
    assert tok.kind is TokenKind.IDENT
    assert tok.text == "foo"
    assert tok.span is span
