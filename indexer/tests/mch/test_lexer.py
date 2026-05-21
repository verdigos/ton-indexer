from __future__ import annotations

import pytest

from indexer.events.mch.lexer import tokenize
from indexer.events.mch.tokens import TokenKind


def _kinds(text: str) -> list[TokenKind]:
    return [t.kind for t in tokenize(text, "f.mch") if t.kind is not TokenKind.EOF]


def test_opcode_declaration():
    toks = list(tokenize("opcode JettonTransfer = 0x0f8a7ea5", "f.mch"))
    assert [t.kind for t in toks] == [
        TokenKind.KW_OPCODE,
        TokenKind.IDENT,
        TokenKind.EQ,
        TokenKind.HEX,
        TokenKind.EOF,
    ]
    assert toks[1].text == "JettonTransfer"
    assert toks[3].text == "0x0f8a7ea5"


def test_identifier_with_underscore_and_digits():
    toks = list(tokenize("foo_bar_42", "f.mch"))
    assert toks[0].kind is TokenKind.IDENT
    assert toks[0].text == "foo_bar_42"


def test_decimal_literal():
    toks = list(tokenize("42", "f.mch"))
    assert toks[0].kind is TokenKind.DEC
    assert toks[0].text == "42"


def test_duration_literal():
    toks = list(tokenize("6h", "f.mch"))
    assert toks[0].kind is TokenKind.DURATION
    assert toks[0].text == "6h"


def test_keywords_promoted_from_idents():
    assert _kinds("op btype pred any maybe where") == [
        TokenKind.KW_OP, TokenKind.KW_BTYPE, TokenKind.KW_PRED,
        TokenKind.KW_ANY, TokenKind.KW_MAYBE, TokenKind.KW_WHERE,
    ]


def test_arrows_and_punctuation():
    assert _kinds("-> <- | @ $ { } ( ) = . ;") == [
        TokenKind.ARROW_R, TokenKind.ARROW_L, TokenKind.PIPE, TokenKind.AT,
        TokenKind.DOLLAR, TokenKind.LBRACE, TokenKind.RBRACE,
        TokenKind.LPAREN, TokenKind.RPAREN, TokenKind.EQ, TokenKind.DOT, TokenKind.SEMI,
    ]


def test_line_comments_consumed():
    src = "opcode Foo = 0x1  # this is a comment\nopcode Bar = 0x2"
    kinds = _kinds(src)
    # NEWLINE emitted between the two declarations
    assert TokenKind.NEWLINE in kinds
    assert kinds.count(TokenKind.KW_OPCODE) == 2


def test_newline_emitted_inside_children_block_context():
    # The lexer always emits NEWLINE tokens; the parser decides where they matter.
    toks = list(tokenize("a\nb", "f.mch"))
    kinds = [t.kind for t in toks]
    assert TokenKind.NEWLINE in kinds


def test_span_tracks_line_and_column():
    toks = list(tokenize("opcode  Foo", "f.mch"))
    foo_tok = next(t for t in toks if t.text == "Foo")
    assert foo_tok.span.start_line == 1
    assert foo_tok.span.start_col == 9
    assert foo_tok.span.end_col == 12


def test_span_advances_across_newline():
    toks = list(tokenize("opcode Foo = 0x1\nopcode Bar = 0x2", "f.mch"))
    bar_tok = next(t for t in toks if t.text == "Bar")
    assert bar_tok.span.start_line == 2
    assert bar_tok.span.start_col == 8


def test_eof_token_always_last():
    toks = list(tokenize("opcode Foo = 0x1", "f.mch"))
    assert toks[-1].kind is TokenKind.EOF


from indexer.events.mch.lexer import LexerError


def test_bad_hex_literal():
    with pytest.raises(LexerError) as exc:
        list(tokenize("opcode Foo = 0x", "f.mch"))
    assert exc.value.code == "L001_BAD_NUMERIC"
    assert exc.value.span.start_line == 1


def test_unknown_character():
    with pytest.raises(LexerError) as exc:
        list(tokenize("opcode ! = 0x1", "f.mch"))
    assert exc.value.code == "L002_UNKNOWN_CHAR"


def test_unterminated_string():
    with pytest.raises(LexerError) as exc:
        list(tokenize('"unterminated', "f.mch"))
    assert exc.value.code == "L003_UNTERMINATED_STRING"


def test_caret_token():
    toks = list(tokenize("^", "<test>"))
    assert toks[0].kind is TokenKind.CARET
    assert toks[0].text == "^"
