from __future__ import annotations

from dataclasses import fields, is_dataclass, replace

import pytest

from indexer.events.mch import ast_
from indexer.events.mch.diagnostics import CompileError
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse


def _parse(src: str) -> ast_.File:
    tokens = list(tokenize(src, "test.mch"))
    return parse(tokens, "test.mch")


def test_parse_opcode_declaration_hex():
    f = _parse("opcode JettonTransfer = 0x0f8a7ea5")
    assert len(f.opcodes) == 1
    assert f.opcodes[0].name == "JettonTransfer"
    assert f.opcodes[0].value == 0x0f8a7ea5


def test_parse_opcode_declaration_decimal():
    f = _parse("opcode Foo = 42")
    assert f.opcodes[0].value == 42


def test_parse_predicate_declaration():
    f = _parse("predicate pton_self_transfer")
    assert len(f.predicates) == 1
    assert f.predicates[0].name == "pton_self_transfer"


def test_parse_rule_declaration_single_node():
    f = _parse("opcode Foo = 0x1\nrule r = op Foo")
    assert len(f.rules) == 1
    assert f.rules[0].name == "r"
    assert isinstance(f.rules[0].pattern, ast_.Sequence) or isinstance(f.rules[0].pattern, ast_.Node)


def test_parse_matcher_declaration_minimal():
    src = """
    opcode Foo = 0x1
    matcher m {
        produces FooBlock

        op Foo @x
        out { v: 1 }
    }
    """
    f = _parse(src)
    assert len(f.matchers) == 1
    m = f.matchers[0]
    assert m.name == "m"
    assert m.produces == ("FooBlock",)
    assert isinstance(m.build_stmts[0], ast_.OutStmt)
    assert m.include_excess is True   # default
    assert m.include_bounces is True  # default


def test_parse_matcher_with_directive_overrides():
    src = """
    opcode Foo = 0x1
    matcher m {
        produces FooBlock
        include_excess  false
        include_bounces false

        op Foo @x
        out { v: 1 }
    }
    """
    f = _parse(src)
    assert f.matchers[0].include_excess is False
    assert f.matchers[0].include_bounces is False


@pytest.mark.parametrize("name", ["stitch", "partial", "final", "key", "ttl", "merge"])
def test_stitch_words_are_plain_identifiers(name):
    node = _parse(f"rule {name} = op A @{name}").rules[0].pattern
    assert node.capture == name


def test_duration_spelling_rejected_as_stray_identifier():
    _parse_expect_error("opcode A = 30s", "P001_UNEXPECTED_TOKEN")


def test_parse_node_op_with_capture():
    src = """
    opcode Foo = 0x1
    rule r = op Foo @x
    """
    f = _parse(src)
    seq = f.rules[0].pattern
    # A bare op-with-capture is just a Node (not wrapped in Sequence).
    assert isinstance(seq, ast_.Node)
    assert seq.capture == "x"
    assert isinstance(seq.head, ast_.OpHead)
    assert seq.head.ref == "Foo"


def test_parse_linear_chain():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    rule r = op A @a -> op B @b -> op C @c
    """
    f = _parse(src)
    seq = f.rules[0].pattern
    assert isinstance(seq, ast_.Sequence)
    assert isinstance(seq.head, ast_.Node) and seq.head.capture == "a"
    assert len(seq.tail) == 2
    assert seq.tail[0][0] is ast_.Edge.CHILD
    assert seq.tail[1][0] is ast_.Edge.CHILD


def test_parse_maybe_wrapped_node():
    src = """
    opcode A = 0x1
    rule r = maybe op A @a
    """
    f = _parse(src)
    seq = f.rules[0].pattern
    assert isinstance(seq, ast_.Maybe)
    assert isinstance(seq.inner, ast_.Node)


def test_parse_peek_and_maybe_compose_in_both_orders():
    src = """
    opcode A = 0x1
    rule outer_maybe = maybe peek op A @a
    rule outer_peek = peek maybe op A @b
    """
    f = _parse(src)

    outer_maybe = f.rules[0].pattern
    assert isinstance(outer_maybe, ast_.Maybe)
    assert isinstance(outer_maybe.inner, ast_.Peek)
    assert isinstance(outer_maybe.inner.inner, ast_.Node)

    outer_peek = f.rules[1].pattern
    assert isinstance(outer_peek, ast_.Peek)
    assert isinstance(outer_peek.inner, ast_.Maybe)
    assert isinstance(outer_peek.inner.inner, ast_.Node)


def test_parse_peek_wraps_one_atom_not_later_chain_atoms():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    rule r = peek op A @a -> op B @b
    """
    seq = _parse(src).rules[0].pattern
    assert isinstance(seq, ast_.Sequence)
    assert isinstance(seq.head, ast_.Peek)
    assert isinstance(seq.head.inner, ast_.Node)
    assert isinstance(seq.tail[0][1], ast_.Node)


def test_parse_peek_accepts_group_rule_ref_and_children_items():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    rule leaf = op B @b
    rule group = peek xor { peek op A @a ; peek op B @b }
    rule ref = peek $leaf
    rule children = op A @a -> { peek op B @b }
    """
    f = _parse(src)
    assert isinstance(f.rules[1].pattern, ast_.Peek)
    assert isinstance(f.rules[1].pattern.inner, ast_.Alternative)
    assert isinstance(f.rules[2].pattern, ast_.Peek)
    assert isinstance(f.rules[2].pattern.inner, ast_.RuleRef)
    children = f.rules[3].pattern
    assert isinstance(children, ast_.Sequence)
    assert isinstance(children.tail[0][1], ast_.ChildrenBlock)
    assert isinstance(children.tail[0][1].items[0], ast_.Peek)


def test_parse_rule_reference():
    src = """
    opcode A = 0x1
    rule inner = op A @a
    rule outer = $inner
    """
    f = _parse(src)
    assert isinstance(f.rules[1].pattern, ast_.RuleRef)
    assert f.rules[1].pattern.name == "inner"


def test_parse_children_block_with_semicolons():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    rule r = op A @a -> { op B @b; op C @c }
    """
    f = _parse(src)
    seq = f.rules[0].pattern
    assert isinstance(seq, ast_.Sequence)
    cb = seq.tail[0][1]
    assert isinstance(cb, ast_.ChildrenBlock)
    assert len(cb.items) == 2


def test_parse_children_block_with_newlines():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    rule r = op A @a -> {
        op B @b
        op C @c
    }
    """
    f = _parse(src)
    seq = f.rules[0].pattern
    cb = seq.tail[0][1]
    assert isinstance(cb, ast_.ChildrenBlock)
    assert len(cb.items) == 2


def test_parse_predicate_clause():
    src = """
    opcode A = 0x1
    predicate my_pred
    rule r = op A @a where my_pred
    """
    f = _parse(src)
    seq = f.rules[0].pattern
    assert isinstance(seq, ast_.Node)
    assert seq.where_predicate == "my_pred"


def test_parse_parent_edge():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    rule r = op A @a <- op B @b
    """
    f = _parse(src)
    seq = f.rules[0].pattern
    assert isinstance(seq, ast_.Sequence)
    assert seq.tail[0][0] is ast_.Edge.PARENT


def _parse_expect_error(src: str, code: str) -> None:
    with pytest.raises(CompileError) as exc:
        _parse(src)
    codes = [d.code for d in exc.value.bag.items]
    assert code in codes, f"expected diagnostic {code}, got {codes}"


def test_unexpected_top_level_token():
    _parse_expect_error("garbage 0x1", "P001_UNEXPECTED_TOKEN")


def test_missing_produces_in_matcher():
    src = """
    opcode Foo = 0x1
    matcher m {
        op Foo @x
    }
    """
    _parse_expect_error(src, "P001_UNEXPECTED_TOKEN")


def test_missing_closing_brace_in_matcher():
    src = """
    opcode Foo = 0x1
    matcher m {
        produces FooBlock

        op Foo @x
    """
    _parse_expect_error(src, "P001_UNEXPECTED_TOKEN")


def _parse_matcher_pattern(src: str) -> ast_.PatternExpr:
    """Parse src wrapped in a matcher decl, return the matcher's pattern."""
    full = (
        "opcode A = 0x1\nopcode B = 0x2\nopcode C = 0x3\n"
        "matcher m {\n  produces MB\n"
        + src + "\nout { v: 1 }\n}"
    )
    f = _parse(full)
    return f.matchers[0].pattern


# ==========================================================================
# T11 — `produces` union (spec §8.1)
# ==========================================================================

def test_produces_single_name_still_works():
    src = """
    opcode Foo = 0x1
    matcher m {
        produces FooBlock

        op Foo @x
        out { v: 1 }
    }
    """
    f = _parse(src)
    assert f.matchers[0].produces == ("FooBlock",)


def test_produces_union_of_three_parses_into_tuple():
    src = """
    opcode Foo = 0x1
    matcher m {
        produces change_dns | renew_dns | delete_dns

        op Foo @x
        out { v: 1 }
    }
    """
    f = _parse(src)
    assert f.matchers[0].produces == ("change_dns", "renew_dns", "delete_dns")


def _build_stmts(src: str):
    return _parse(
        "matcher m { produces MB\n op 1 @a\n" + src + "\n}"
    ).matchers[0].build_stmts


def _without_spans(node):
    if isinstance(node, tuple):
        return tuple(_without_spans(item) for item in node)
    if is_dataclass(node):
        return replace(node, **{
            f.name: None if f.name == "span" else False if f.name == "opcode_set"
            else _without_spans(getattr(node, f.name))
            for f in fields(node)
        })
    return node


@pytest.mark.parametrize("src", [
    "or { op A ; op B }",
    "xor { op A @x ; op B @x }",
    "or { ;\n op A\n op B\n op C;\n }",
    "maybe or { op A ; op B } -> op T",
    "peek or { op A ; op B }",
    "op R -> or { op A ; op B } -> op T",
    "op R <- or { op A ; op B }",
    "op R -> { or { op A ; op B } ; op T }",
    "op R { <- or { op A ; op B } }",
    "op R { -> or { op A ; op B } }",
    "op R { -> { or { op A ; op B } ; op T } }",
    "or { xor { op A ; op B } ; op C }",
    "or { op A -> op B ; op C }",
])
def test_alternative_block_replaces_operators(src):
    _parse_matcher_pattern(src)
    _parse("rule r = " + src)


@pytest.mark.parametrize("head", ["or", "xor"])
def test_alternative_block_span_and_flags(head):
    block = head + " { op A ; op B }"
    src = "rule r = maybe " + block + " -> op T"
    alternative = _parse(src).rules[0].pattern.head.inner
    assert isinstance(alternative, ast_.Alternative)
    assert alternative.exclusive is (head == "xor")
    assert alternative.opcode_set is False
    assert src[alternative.span.start_off:alternative.span.end_off] == block


@pytest.mark.parametrize("head", ["or", "xor"])
@pytest.mark.parametrize("arms", ["", ";\n;", "op A", ";\n op A ;\n"])
def test_alternative_block_requires_two_arms(head, arms):
    block = head + " { " + arms + " }"
    src = "rule r = " + block
    with pytest.raises(CompileError) as exc:
        _parse(src)
    diagnostic, = exc.value.bag.items
    assert diagnostic.code == "P021_CHOICE_NEEDS_TWO_ARMS"
    assert diagnostic.message == "or/xor block needs at least two arms"
    assert src[diagnostic.span.start_off:diagnostic.span.end_off] == block


@pytest.mark.parametrize("pattern", [
    "or { op A ; op B }",
    "xor { op A ; op B }",
    "$rule",
])
@pytest.mark.parametrize("where", ["my_pred", "(.x == 1)"])
def test_where_after_choice_or_rule_ref_rejected(pattern, where):
    _parse_expect_error(f"{pattern} where {where}", "P001_UNEXPECTED_TOKEN")


@pytest.mark.parametrize("name", ["or", "xor"])
def test_alternative_words_remain_names(name):
    node = _parse_matcher_pattern(f"op A @{name}")
    assert node.capture == name
    assert _build_stmts(f"let {name} = 1")[0].name == name


@pytest.mark.parametrize("head", ["choice", "choice head_unique", "head_unique"])
def test_removed_alternative_spelling_rejected(head):
    _parse_expect_error("rule r = " + head + " { op A; op B }", "P001_UNEXPECTED_TOKEN")


def test_or_remains_boolean_operator():
    where = _parse_matcher_pattern("op A where (.x == 1 or .x == 2)").where_expr
    build = _expr("a.x == 1 or a.x == 2")
    for expr in (where, build):
        assert isinstance(expr, ast_.BinaryOp)
        assert expr.op == "or"
        assert expr.left.op == expr.right.op == "=="


@pytest.mark.parametrize("src, expanded", [
    ("op [A, B] @x", "xor { op A @x ; op B @x }"),
    ("op [A, B] @x -> op T", "xor { op A @x ; op B @x } -> op T"),
    ("op R -> op [A, B] @x -> op T",
     "op R -> xor { op A @x -> op T ; op B @x -> op T }"),
    ("op R -> maybe op [A, B] @x -> { op T ; op U }",
     "op R -> maybe xor { op A @x -> { op T ; op U } ; op B @x -> { op T ; op U } }"),
    ("op R -> peek op [A, B]", "op R -> peek xor { op A ; op B }"),
    ("op [A, B, C] @x -> op T <- op P",
     "xor { op A @x ; op B @x ; op C @x } -> op T <- op P"),
    ("maybe peek op [A, B] @x -> op T",
     "maybe peek xor { op A @x ; op B @x } -> op T"),
    ("peek maybe op [A, B] @x -> op T",
     "peek maybe xor { op A @x ; op B @x } -> op T"),
    ("op [A, B] -> op [T, U]",
     "xor { op A ; op B } -> xor { op T ; op U }"),
    ("op [A, B] -> op [T, U] -> op V",
     "xor { op A ; op B } -> xor { op T -> op V ; op U -> op V }"),
    ("(op [A, B] @x) -> op T", "xor { op A @x ; op B @x } -> op T"),
    ("(op [A, B] @x -> op T)", "xor { op A @x -> op T ; op B @x -> op T }"),
    ("op R -> { op [A, B] @x ; op T }", "op R -> { xor { op A @x ; op B @x } ; op T }"),
    ("op R -> { op [A, B] @x -> op T }",
     "op R -> { xor { op A @x -> op T ; op B @x -> op T } }"),
    ("op [A] @x", "op A @x"),
    ("op [A]", "op A"),
    ("maybe op [A] -> op T", "maybe op A -> op T"),
    ("op [A, 0x2, 3,]", "xor { op A ; op 0x2 ; op 3 }"),
    ("op [\nA\n,\nB,\n] @x", "xor { op A @x ; op B @x }"),
])
def test_opcode_set_desugars(src, expanded):
    assert _without_spans(_parse_matcher_pattern(src)) == _without_spans(
        _parse_matcher_pattern(expanded)
    )


@pytest.mark.parametrize("src, expanded", [
    ("op [A, B] @x -> op T", "xor { op A @x -> op T ; op B @x -> op T }"),
    ("op [A, B, C] @x -> op T <- op P",
     "xor { op A @x -> op T <- op P ; op B @x -> op T <- op P ; op C @x -> op T <- op P }"),
    ("maybe peek op [A, B] @x -> op T",
     "maybe peek xor { op A @x -> op T ; op B @x -> op T }"),
    ("peek maybe op [A, B] @x -> op T",
     "peek maybe xor { op A @x -> op T ; op B @x -> op T }"),
    ("op [A, B] -> op [T, U]",
     "xor { op A -> xor { op T ; op U } ; op B -> xor { op T ; op U } }"),
])
def test_opcode_set_rule_distributes_tail(src, expanded):
    assert _without_spans(_parse("rule r = " + src).rules[0].pattern) == _without_spans(
        _parse("rule r = " + expanded).rules[0].pattern
    )


def test_opcode_set_copies_nested_tail_objects():
    pattern = _parse("rule r = op [A, B] @x -> { xor { op T @t ; op U @t } ; op V }").rules[0].pattern
    assert isinstance(pattern, ast_.Alternative)
    assert pattern.opcode_set is True
    assert pattern.exclusive is True
    left, right = pattern.branches
    assert left.head is not right.head
    assert left.head.head is not right.head.head
    assert left.head.span == right.head.span
    assert left.head.span.end_off - left.head.span.start_off == len("op [A, B] @x")
    left_block, right_block = left.tail[0][1], right.tail[0][1]
    assert left_block is not right_block
    for a, b in zip(left_block.items, right_block.items):
        assert a is not b
    for a, b in zip(left_block.items[0].branches, right_block.items[0].branches):
        assert a is not b
        assert a.head is not b.head


def test_parenthesized_opcode_set_keeps_marker():
    pattern = _parse_matcher_pattern("(op [A, B])")
    assert pattern.opcode_set is True
    assert pattern.exclusive is True
    assert all(isinstance(branch, ast_.Node) for branch in pattern.branches)


def _expr(src: str):
    return _build_stmts("let v = " + src)[0].value


@pytest.mark.parametrize("src", [
    "a +\n b *\n c",
    "a and\n not\n b",
    "a ??\n -\n b",
    "a if\n c\n else\n b",
    "(a\n + b)",
    "(a\n if c else b)",
    "(a if c\n else b)",
    "[1\n, 2\n]",
    "[1,\n 2]",
    "(\n(a\n + (b\n * c\n))\n - d\n)",
    "(a if\n c else b)",
    "(a if c else\n b)",
    "(a if\n not c\n else\n f(b))",
    "a if c else (\n f(b) if d\n else g(b))",
    "(a and\n not b)",
    "(a or\n b)",
    "(a ??\n -b)",
    "(a +\n b *\n c -\n d)",
    "(a ==\n b)",
    "(f(\n a,\n [\n b,\n c]))",
    "\n (a)",
])
def test_open_expression_continues_across_newlines(src):
    assert _without_spans(_expr(src)) == _without_spans(_expr(src.replace("\n", " ")))


@pytest.mark.parametrize("call", ["f({})", "map(xs as x => {})"])
def test_record_newline_separators_inside_parentheses(call):
    actual = call.format("{\n a: (1\n + 2)\n b: [3\n, 4]\n}\n")
    expected = call.format("{a: (1 + 2); b: [3, 4]}")
    assert _without_spans(_expr(actual)) == _without_spans(_expr(expected))


@pytest.mark.parametrize("src", [
    "let v = a\n + b",
    "let v = (a)\n + b",
    "out { v: (a)\n + b }",
])
def test_closed_expression_line_rejects_continuation(src):
    with pytest.raises(CompileError) as exc:
        _build_stmts(src)
    assert exc.value.bag.items[0].code == "P001_UNEXPECTED_TOKEN"


@pytest.mark.parametrize("src, message", [
    (")", "expected top-level declaration, found RPAREN (')')"),
    ("]", "expected top-level declaration, found RBRACKET (']')"),
    ("}", "expected top-level declaration, found RBRACE ('}')"),
    ("fn f() = (a]", "expected RPAREN, found RBRACKET (']')"),
    ("fn f() = [1)", "expected RBRACKET, found RPAREN (')')"),
])
def test_unbalanced_closers_keep_parser_diagnostics(src, message):
    with pytest.raises(CompileError) as exc:
        _parse(src)
    diagnostic, = exc.value.bag.items
    assert diagnostic.code == "P001_UNEXPECTED_TOKEN"
    assert diagnostic.message == message
    assert src[diagnostic.span.start_off:diagnostic.span.end_off] == src[-1]


def test_require_desugars_to_let_and_reject():
    src = "require x = lookup foo(a)"
    stmts = _build_stmts(src)
    assert [type(stmt) for stmt in stmts] == [ast_.LetStmt, ast_.RejectStmt]
    assert _without_spans(stmts) == _without_spans(
        _build_stmts("let x = lookup foo(a)\nreject when x == null")
    )
    assert stmts[0].span == stmts[1].span
    assert stmts[0].span.end_off - stmts[0].span.start_off == len(src)


def test_require_is_hard_keyword():
    with pytest.raises(CompileError):
        _build_stmts("let require = 1")


@pytest.mark.parametrize("src, expanded", [
    ("x in [1, 2, 3]", "x == 1 or x == 2 or x == 3"),
    ("x not in [1, 2, 3]", "x != 1 and x != 2 and x != 3"),
    ("x in []", "false"),
    ("x not in []", "true"),
    ("not x in [1, 2]", "not (x == 1 or x == 2)"),
    ("r.body.value in [true, false, null,]",
     "r.body.value == true or r.body.value == false or r.body.value == null"),
    ("x in [\n0x1,\n]", "x == 1"),
    ('.comment in ["finish", "stop"]', '.comment == "finish" or .comment == "stop"'),
    (".data.op not in [1, 2]", ".data.op != 1 and .data.op != 2"),
])
def test_membership_desugars(src, expanded):
    assert _without_spans(_expr(src)) == _without_spans(_expr(expanded))


def test_membership_literals_and_shared_left_operand():
    expr = _expr('x not in [1, -2, "s", null]')
    comparisons = [expr.left.left.left, expr.left.left.right, expr.left.right, expr.right]
    left = comparisons[0].left
    assert isinstance(left, ast_.NameRef) and left.name == "x"
    assert all(c.op == "!=" and c.left is left for c in comparisons)
    assert [type(c.right) for c in comparisons] == [
        ast_.IntLit, ast_.IntLit, ast_.StrLit, ast_.NullLit,
    ]
    assert [c.right.value for c in comparisons[:3]] == [1, -2, "s"]


@pytest.mark.parametrize("src, code", [
    ("x in [1] == true", "P001_UNEXPECTED_TOKEN"),
    ("x == 1 in [true]", "P001_UNEXPECTED_TOKEN"),
    ("x in [1] in [true]", "P001_UNEXPECTED_TOKEN"),
])
def test_membership_errors(src, code):
    with pytest.raises(CompileError) as exc:
        _expr(src)
    assert code in [d.code for d in exc.value.bag.items]


def test_in_remains_identifier():
    stmts = _build_stmts("let in = 1\nlet v = r.in\nout { in: 1 }")
    assert stmts[0].name == "in"
    assert isinstance(stmts[1].value, ast_.FieldAccess)
    assert stmts[1].value.field == "in"
    assert stmts[2].fields[0].name == "in"
    assert _expr("{in: 1}").fields[0].name == "in"


def test_nullable_parse_statement_alias():
    assert _without_spans(_build_stmts("parse? a as T | U")) == _without_spans(
        _build_stmts("parse a as T | U")
    )


@pytest.mark.parametrize("src, nullable", [
    ("parse? a.body as T | U", True),
    ("parse! a.body as T | U", False),
])
def test_parse_expression_nullability(src, nullable):
    expr = _expr(src)
    assert isinstance(expr, ast_.ParseExpr) and expr.nullable is nullable
    assert expr.msg_types == ("T", "U")
    assert isinstance(expr.target, ast_.FieldAccess) and expr.target.field == "body"


def test_try_is_an_identifier():
    assert _parse_matcher_pattern("op A @try").capture == "try"
    assert _build_stmts("let try = 1")[0].name == "try"
    assert _expr("try").name == "try"


@pytest.mark.parametrize("src", ["parse ? a as T", "let v = parse ? a as T"])
def test_parse_with_separated_question_mark_rejected(src):
    with pytest.raises(CompileError):
        _build_stmts(src)


@pytest.mark.parametrize("separator", [";", "\n", ";\n;"])
def test_relation_items_parse_in_any_order(separator):
    clauses = [
        "<- op P <- op Q",
        "-> { op X ; maybe op Y }",
        "-> \n op B -> op C \n",
    ]
    node = _parse_matcher_pattern("op A @a where p { " + separator.join(clauses) + " }")
    assert isinstance(node, ast_.Node)
    assert node.capture == "a" and node.where_predicate == "p"
    r = node.relations
    assert isinstance(r, ast_.Relations)
    for target, spelling in [
        (r.children_all, "op B -> op C"),
        (r.children_first, "op X ; maybe op Y"),
        (r.parent, "op P <- op Q"),
    ]:
        expected = (_parse_matcher_pattern("{ " + spelling + " }").items
                    if isinstance(target, tuple) else _parse_matcher_pattern(spelling))
        assert _without_spans(target) == _without_spans(expected)
    reversed_node = _parse_matcher_pattern("op A @a where p { " + separator.join(reversed(clauses)) + " }")
    assert _without_spans(node) == _without_spans(reversed_node)
    assert node.span.end_off == r.span.end_off


@pytest.mark.parametrize("head", ["op A", "btype thing", "pred p", "any", "op A where (.x == 1)"])
def test_relations_follow_each_node_head(head):
    node = _parse_matcher_pattern(head + " { <- op P }")
    assert node.relations.children_all is None
    assert node.relations.children_first is None
    assert node.relations.parent.head.ref == "P"


def test_opcode_set_copies_relations_and_nested_patterns():
    pattern = _parse_matcher_pattern(
        "op [A, B] @x { -> op C; -> { op D ; maybe op E }; <- op P }"
    )
    left, right = pattern.branches
    assert left.relations is not right.relations
    assert left.relations == right.relations
    assert left.relations.children_all is not right.relations.children_all
    assert left.relations.parent is not right.relations.parent
    for a, b in zip(left.relations.children_first, right.relations.children_first):
        assert a is not b


@pytest.mark.parametrize("arrow", ["->", "<-"])
@pytest.mark.parametrize("separator", [";", "\n"])
def test_duplicate_relation_item_rejected(arrow, separator):
    with pytest.raises(CompileError) as exc:
        _parse_matcher_pattern(f"op A {{ {arrow} op X {separator} {arrow} op Y }}")
    diagnostic, = exc.value.bag.items
    assert diagnostic.code == "P018_DUPLICATE_RELATION_CLAUSE"
    if arrow == "->":
        assert diagnostic.message == "one child chain per node; use a children set `-> { … }` for several children"


@pytest.mark.parametrize("separator", [";", "\n"])
def test_relation_children_sets_concatenate(separator):
    actual = _parse_matcher_pattern(
        "op A { -> { op X; maybe op Y }" + separator + "-> { op Z } }"
    ).relations
    expected = _parse_matcher_pattern("op A { -> { op X; maybe op Y; op Z } }").relations
    assert _without_spans(actual) == _without_spans(expected)


def test_multiline_relation_items():
    actual = _parse_matcher_pattern("""op A {
        ->
            op B ->
            op C
        -> {
            op X
            maybe op Y
        }
        <-
            (
                op P
                <- op Q
            )
    }""")
    expected = _parse_matcher_pattern(
        "op A { -> op B -> op C; -> { op X; maybe op Y }; <- (op P <- op Q) }"
    )
    assert _without_spans(actual) == _without_spans(expected)


@pytest.mark.parametrize("target", ["maybe op X", "peek op X"])
def test_relation_arrow_modifiers(target):
    relations = _parse_matcher_pattern("op A { -> " + target + " }").relations
    assert _without_spans(relations.children_all) == _without_spans(_parse_matcher_pattern(target))
    assert relations.children_first is None


@pytest.mark.parametrize("left", [
    "op A { -> { op X } }",
    "(op A { -> { op X } })",
    "maybe peek ((op A { <- op P }))",
    "(op R -> op A { -> op X })",
    "op [A, B] { -> { op X } }",
    "xor { op A { -> { op X } } ; op B }",
])
@pytest.mark.parametrize("edge", ["->", "<-"])
def test_arrow_after_relation_block_rejected(left, edge):
    with pytest.raises(CompileError) as exc:
        _parse_matcher_pattern(left + "\n" + edge + " op Y")
    diagnostic, = exc.value.bag.items
    assert diagnostic.code == "P019_ARROW_AFTER_RELATION_BLOCK"
    assert diagnostic.message == "relations of this node are declared in its block"


def test_next_line_brace_is_not_a_relation_block():
    with pytest.raises(CompileError) as exc:
        _parse_matcher_pattern("op A\n{ -> op X }")
    diagnostic, = exc.value.bag.items
    assert diagnostic.code == "P001_UNEXPECTED_TOKEN"
    assert "found LBRACE" in diagnostic.message
    block = _parse_matcher_pattern("{ op A\n{ op X } }")
    assert block.items[0].relations is None
    assert isinstance(block.items[1], ast_.ChildrenBlock)


@pytest.mark.parametrize("block", [
    "{}", "{ op X }", "{ -> { op X } op Y }",
    "{ -> op X ; op Y }", "{ <- op X ; op Y }",
    "{ -> }", "{ <- }",
])
def test_invalid_relation_blocks_rejected(block):
    with pytest.raises(CompileError):
        _parse_matcher_pattern("op A " + block)


@pytest.mark.parametrize("suffix, types, predicate", [
    ("as T", ("T",), None),
    ("as T1 | T2 where p", ("T1", "T2"), "p"),
    ("as T { -> { op B } }", ("T",), None),
])
def test_node_parse_annotation(suffix, types, predicate):
    src = "rule r = op A @x " + suffix
    node = _parse(src).rules[0].pattern
    assert node.capture == "x"
    assert node.parse_types == types
    assert node.where_predicate == predicate
    assert src[node.span.start_off:node.span.end_off] == "op A @x " + suffix
    if node.relations is not None:
        child, = node.relations.children_first
        assert child.head.ref == "B"
        assert child.parse_types is None


@pytest.mark.parametrize("right", [
    "op B @y as T2", "btype b", "pred p", "any", "maybe op B",
    "peek op B", "$r", "(op B)",
])
def test_node_parse_annotation_stops_at_pattern_alternative(right):
    pattern = _parse("rule r = or { op A @x as T1 ; " + right + " }").rules[0].pattern
    left, second = pattern.branches
    assert left.parse_types == ("T1",)
    if right == "op B @y as T2":
        assert second.capture == "y"
        assert second.parse_types == ("T2",)


@pytest.mark.parametrize("name, params, body", [
    ("f", ("x",), "x + 1"),
    ("g", (), "1"),
    ("h", ("a", "b"), "a if a > b else b"),
])
def test_fn_declaration(name, params, body):
    src = f"fn {name}({', '.join(params)}) = {body}"
    fn, = _parse(src).fns
    assert isinstance(fn, ast_.FnDecl)
    assert fn.name == name and fn.params == params
    assert _without_spans(fn.body) == _without_spans(_expr(body))
    assert src[fn.span.start_off:fn.span.end_off] == src


def test_fn_between_declarations_and_fn_remains_identifier():
    file = _parse(
        "opcode fn = 1\nfn f(x) = x\npredicate p\nfn g() = 1\n"
        "rule r = op fn\n"
        "matcher m { produces MB\n op fn @fn\n out { fn: f(fn) } }\n"
        "fn h(a, b) = a if a > b else b"
    )
    assert [fn.name for fn in file.fns] == ["f", "g", "h"]
    assert file.opcodes[0].name == "fn"
    assert file.predicates[0].name == "p"
    assert file.rules[0].name == "r"
    assert file.matchers[0].pattern.capture == "fn"
    assert _build_stmts("let fn = 1")[0].name == "fn"
    assert _expr("fn(1)").callee.name == "fn"


def test_entry_directive_rejected():
    _parse_expect_error(
        "matcher m { produces MB\n entry @x\n op 1 @x\n out { v: 1 } }",
        "P001_UNEXPECTED_TOKEN",
    )


@pytest.mark.parametrize("expr", ["x.f(a)", "(g)(a)", "((g))(a)", "g(a)(a)", "1(a)"])
def test_call_on_non_name_callee_rejected(expr):
    with pytest.raises(CompileError) as exc:
        _expr(expr)
    diagnostic, = exc.value.bag.items
    assert diagnostic.code == "P025_CALLEE_NOT_NAME"
    assert diagnostic.message == "only a function name can be called"
