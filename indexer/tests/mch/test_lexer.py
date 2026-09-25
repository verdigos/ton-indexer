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


@pytest.mark.parametrize("unit", ["s", "m", "h", "d"])
def test_duration_spelling_is_decimal_then_identifier(unit):
    toks = list(tokenize("30" + unit, "f.mch"))
    assert [t.kind for t in toks] == [TokenKind.DEC, TokenKind.IDENT, TokenKind.EOF]
    assert [t.text for t in toks] == ["30", unit, ""]


def test_keywords_promoted_from_idents():
    assert _kinds("op btype pred any maybe peek where") == [
        TokenKind.KW_OP, TokenKind.KW_BTYPE, TokenKind.KW_PRED,
        TokenKind.KW_ANY, TokenKind.KW_MAYBE, TokenKind.KW_PEEK, TokenKind.KW_WHERE,
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
    toks = list(tokenize("{a\nb}", "f.mch"))
    kinds = [t.kind for t in toks]
    assert TokenKind.NEWLINE in kinds


@pytest.mark.parametrize("opening, closing", [("(", ")"), ("[", "]")])
def test_innermost_bracket_controls_newlines(opening, closing):
    src = opening + "\n{\n[\n1\n]\n}\n" + closing + "\n"
    toks = list(tokenize(src, "f.mch"))
    assert [t.span.start_line for t in toks if t.kind is TokenKind.NEWLINE] == [2, 5, 7]
    assert next(t for t in toks if t.text == "1").span.start_line == 4


def test_comments_and_strings_do_not_change_bracket_context():
    assert _kinds('(a # }\n + "[")\nb') == [
        TokenKind.LPAREN, TokenKind.IDENT, TokenKind.PLUS, TokenKind.STRING,
        TokenKind.RPAREN, TokenKind.NEWLINE, TokenKind.IDENT,
    ]


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


def test_unknown_character():
    with pytest.raises(LexerError) as exc:
        list(tokenize("opcode ! = 0x1", "f.mch"))
    assert exc.value.code == "L002_UNKNOWN_CHAR"


def test_unterminated_string():
    with pytest.raises(LexerError) as exc:
        list(tokenize('"unterminated', "f.mch"))
    assert exc.value.code == "L003_UNTERMINATED_STRING"


def test_caret_is_unknown_character():
    with pytest.raises(LexerError) as exc:
        list(tokenize("^", "<test>"))
    assert exc.value.code == "L002_UNKNOWN_CHAR"


@pytest.mark.parametrize("src, expected", [
    ("parse?x", [TokenKind.KW_PARSE_NULLABLE, TokenKind.IDENT]),
    ("parse? x", [TokenKind.KW_PARSE_NULLABLE, TokenKind.IDENT]),
    ("parse ? x", [TokenKind.KW_PARSE, TokenKind.QUESTION, TokenKind.IDENT]),
    ("parse!x", [TokenKind.KW_PARSE_STRICT, TokenKind.IDENT]),
    ("a != b", [TokenKind.IDENT, TokenKind.NEQ, TokenKind.IDENT]),
    ("parse", [TokenKind.KW_PARSE]),
    ("try", [TokenKind.IDENT]),
    ("build", [TokenKind.IDENT]),
    ("require in Excess Bounce", [TokenKind.KW_REQUIRE] + [TokenKind.IDENT] * 3),
])
def test_build_spellings_tokens(src, expected):
    assert _kinds(src) == expected


@pytest.mark.parametrize("spelling", ["parse?", "parse!"])
def test_parse_suffix_in_token_text_and_span(spelling):
    tok, target, _ = tokenize(spelling + "x", "f.mch")
    assert tok.text == spelling
    assert (tok.span.start_off, tok.span.end_off) == (0, len(spelling))
    assert tok.span.end_col == target.span.start_col == len(spelling) + 1
