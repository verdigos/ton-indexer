from __future__ import annotations

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
        build    build_foo
        entry    @x

        op Foo @x
    }
    """
    f = _parse(src)
    assert len(f.matchers) == 1
    m = f.matchers[0]
    assert m.name == "m"
    assert m.produces == ("FooBlock",)
    assert m.produces_primary == "FooBlock"
    assert m.build == "build_foo"
    assert m.entry == "x"
    assert m.include_excess is True   # default
    assert m.include_bounces is True  # default


def test_parse_matcher_with_directive_overrides():
    src = """
    opcode Foo = 0x1
    matcher m {
        produces FooBlock
        build    build_foo
        entry    @x
        include_excess  false
        include_bounces false

        op Foo @x
    }
    """
    f = _parse(src)
    assert f.matchers[0].include_excess is False
    assert f.matchers[0].include_bounces is False


def test_parse_stitch_declaration():
    src = """
    stitch s {
        partial p
        final   f
        key     @topup.destination
        ttl     6h
        merge   merge_fn
    }
    """
    f = _parse(src)
    assert len(f.stitches) == 1
    s = f.stitches[0]
    assert s.partial == "p"
    assert s.final == "f"
    assert s.key.root_capture == "topup"
    assert s.key.attrs == ("destination",)
    assert s.ttl_value == 6
    assert s.ttl_unit == "h"
    assert s.merge == "merge_fn"


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


def test_parse_alternative_in_group():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    rule r = ( op A @a | op B @b )
    """
    f = _parse(src)
    seq = f.rules[0].pattern
    assert isinstance(seq, ast_.Alternative)
    assert len(seq.branches) == 2


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


def test_missing_produces_or_build_in_matcher():
    src = """
    opcode Foo = 0x1
    matcher m {
        entry @x
        op Foo @x
    }
    """
    _parse_expect_error(src, "P001_UNEXPECTED_TOKEN")


def test_empty_children_block_rejected():
    src = """
    opcode A = 0x1
    rule r = op A @a -> {}
    """
    _parse_expect_error(src, "P002_EMPTY_CHILDREN_ITEM")


def test_missing_closing_brace_in_matcher():
    src = """
    opcode Foo = 0x1
    matcher m {
        produces FooBlock
        build    build_foo
        entry    @x

        op Foo @x
    """
    _parse_expect_error(src, "P001_UNEXPECTED_TOKEN")


# ==========================================================================
# Phase 7 — CARET separator
# ==========================================================================

def _parse_matcher_pattern(src: str) -> ast_.PatternExpr:
    """Parse src wrapped in a matcher decl, return the matcher's pattern."""
    full = (
        "opcode A = 0x1\nopcode B = 0x2\nopcode C = 0x3\n"
        "matcher m {\n  produces MB\n  build b\n  entry @a\n"
        + src + "\n}"
    )
    f = _parse(full)
    return f.matchers[0].pattern


def test_pipe_builds_non_exclusive_alternative():
    node = _parse_matcher_pattern("(op A | op B)")
    assert isinstance(node, ast_.Alternative)
    assert node.exclusive is False


def test_caret_separator_sets_exclusive_true():
    node = _parse_matcher_pattern("(op A ^ op B)")
    assert isinstance(node, ast_.Alternative)
    assert node.exclusive is True
    assert len(node.branches) == 2


def test_caret_three_branches():
    node = _parse_matcher_pattern("(op A ^ op B ^ op C)")
    assert isinstance(node, ast_.Alternative)
    assert node.exclusive is True
    assert len(node.branches) == 3


def test_mixed_separator_emits_p010():
    src = (
        "opcode A = 0x1\nopcode B = 0x2\nopcode C = 0x3\n"
        "matcher m {\n  produces MB\n  build b\n  entry @a\n"
        "(op A ^ op B | op C)\n}"
    )
    _parse_expect_error(src, "P010_ALTERNATIVE_SEPARATOR_MIX")


def test_caret_spanning_newlines():
    node = _parse_matcher_pattern("(\n  op A\n  ^\n  op B\n)")
    assert isinstance(node, ast_.Alternative)
    assert node.exclusive is True


# ==========================================================================
# T11 — `produces` union (spec §8.1)
# ==========================================================================

def test_produces_single_name_still_works():
    src = """
    opcode Foo = 0x1
    matcher m {
        produces FooBlock
        build    build_foo
        entry    @x

        op Foo @x
    }
    """
    f = _parse(src)
    assert f.matchers[0].produces == ("FooBlock",)


def test_produces_union_of_three_parses_into_tuple():
    src = """
    opcode Foo = 0x1
    matcher m {
        produces change_dns | renew_dns | delete_dns
        build    build_foo
        entry    @x

        op Foo @x
    }
    """
    f = _parse(src)
    assert f.matchers[0].produces == ("change_dns", "renew_dns", "delete_dns")
    assert f.matchers[0].produces_primary == "change_dns"
