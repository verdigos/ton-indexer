from __future__ import annotations

import pytest

from indexer.events.mch.diagnostics import CompileError
from indexer.events.mch.lexer import tokenize
from indexer.events.mch.parser import parse
from indexer.events.mch.registry import Registries
from indexer.events.mch.resolver import (
    CaptureInfo,
    RecursionClass,
    ResolvedFile,
    resolve,
)


def _resolve(src: str, registries: Registries | None = None) -> ResolvedFile:
    tokens = list(tokenize(src, "test.mch"))
    f = parse(tokens, "test.mch")
    return resolve(f, registries or Registries())


def test_resolve_opcode_declaration():
    rf = _resolve("opcode JettonTransfer = 0x0f8a7ea5")
    assert rf.opcodes["JettonTransfer"] == 0x0f8a7ea5


@pytest.mark.parametrize("name, value", [("Excess", 0xd53276db), ("Bounce", 0xffffffff)])
def test_builtin_opcode_resolves_without_declaration(name, value):
    rf = _resolve(
        f"matcher m {{ produces FB\n op {name} @a\n out {{ v: 1 }} }}",
        _fb_registries(),
    )
    assert rf.opcodes[name] == value


def test_builtin_opcode_mismatch_errors():
    _resolve_expect_error("opcode Bounce = 0", "R026_BUILTIN_OPCODE_MISMATCH")


def test_builtin_opcode_user_duplicate_errors():
    _resolve_expect_error(
        "opcode Excess = 0xd53276db\nopcode Excess = 0xd53276db",
        "R005_DUP_CAPTURE",
    )


def test_opcode_set_distinct_members_resolve():
    reg = _fb_registries()
    reg.opcodes["B"] = 2
    rf = _resolve(
        "opcode A = 1\nmatcher m { produces FB\n"
        "op [A, B] @x\n out { v: 1 } }", reg,
    )
    assert rf.matchers[0].anchor_group.opcode_set is True
    assert rf.matchers[0].captures["x"].optional is False


def test_opcode_set_unknown_member_is_reported():
    _resolve_expect_error("rule r = op [Missing, 1]", "R001_UNKNOWN_OPCODE")


def test_opcode_set_nested_tail_captures_resolve():
    rf = _resolve(
        "matcher m { produces FB\n"
        "op 9 @root -> op [1, 2] @x -> xor { op 3 @t ; op 4 @t }\n out { v: 1 } }",
        _fb_registries(),
    )
    assert set(rf.matchers[0].captures) == {"root", "x", "t"}


@pytest.mark.parametrize("stmts", [
    "require x = 1\nrequire x = 1",
    "require a = 1",
    "require anchor = 1",
    "require consumed = 1",
])
def test_require_keeps_let_name_protection(stmts):
    _resolve_expect_error(
        "matcher m { produces FB\n op 1 @a\n"
        + stmts + "\nout { v: 1 } }",
        "R013_DUP_LET", _fb_registries(),
    )


def test_resolve_opcode_value_matches_host():
    reg = Registries()
    reg.opcodes["JettonTransfer"] = 0x0f8a7ea5
    rf = _resolve("opcode JettonTransfer = 0x0f8a7ea5", reg)
    assert rf.opcodes["JettonTransfer"] == 0x0f8a7ea5


def _fb_registries() -> Registries:
    return Registries()


def test_resolve_matcher_captures_inferred():
    src = """
    opcode Foo = 0x1
    opcode Bar = 0x2
    matcher m {
        produces FB

        op Foo @a -> op Bar @b
        out { v: 1 }
    }
    """
    rf = _resolve(src, _fb_registries())
    m = rf.matchers[0]
    assert set(m.captures.keys()) == {"a", "b"}
    ci_a = m.captures["a"]
    assert ci_a.optional is False
    assert ci_a.peek is False
    assert ci_a.list_binding is False


def test_resolve_maybe_marks_capture_optional():
    src = """
    opcode Foo = 0x1
    opcode Bar = 0x2
    matcher m {
        produces FB

        op Foo @a -> maybe op Bar @b
        out { v: 1 }
    }
    """
    rf = _resolve(src, _fb_registries())
    m = rf.matchers[0]
    assert m.captures["a"].optional is False
    assert m.captures["b"].optional is True


def test_resolve_peek_marks_capture_without_making_it_optional():
    src = """
    opcode Foo = 0x1
    opcode Bar = 0x2
    matcher m {
        produces FB

        op Foo @a -> peek op Bar @b
        out { v: 1 }
    }
    """
    rf = _resolve(src, _fb_registries())
    m = rf.matchers[0]
    assert m.captures["a"].peek is False
    assert m.captures["b"].peek is True
    assert m.captures["b"].optional is False


def test_resolve_anchor_resolved():
    src = """
    opcode Foo = 0x1
    matcher m {
        produces FB

        op Foo @a
        out { v: 1 }
    }
    """
    rf = _resolve(src, _fb_registries())
    assert rf.matchers[0].anchor_capture == "a"
    # And the anchor head IS the OpHead for Foo:
    anchor_node = rf.matchers[0].anchor_node
    # Inline import to avoid polluting test module namespace:
    from indexer.events.mch import ast_
    assert isinstance(anchor_node.head, ast_.OpHead)
    assert anchor_node.head.ref == "Foo"


def test_resolve_non_recursive_rule_classified():
    src = """
    opcode Foo = 0x1
    rule r = op Foo @x
    """
    rf = _resolve(src)
    assert rf.rule_recursion["r"] is RecursionClass.NONE


def test_resolve_direct_recursive_rule_classified():
    src = """
    opcode A = 0x1
    rule r = op A @a -> maybe $r
    """
    rf = _resolve(src)
    assert rf.rule_recursion["r"] is RecursionClass.DIRECT_RECURSIVE


@pytest.mark.parametrize(("pattern", "host_registered"), [
    ("op 1 @root where missing", False),
    ("op 1 @root where missing", True),
    ("op 1 @root -> maybe op 2 where missing", False),
    ("op 1 @root -> peek op 2 where missing", False),
    ("op 1 @root -> { many op 2 where missing }", False),
    ("op 1 @root -> op 2 where missing", False),
    ("op 1 @root <- op 2 where missing", False),
    ("op 1 @root { -> op 2 where missing }", False),
    ("op 1 @root { -> { op 2 where missing } }", False),
    ("op 1 @root { <- op 2 where missing }", False),
    ("op 1 @root -> or { op 2 where missing; op 3 }", False),
    ("op 1 @root -> xor { op 2 where missing; op 3 }", False),
    ("op 1 @root -> (op 2 where missing)", False),
    ("xor { op 1 @root where missing; op 2 @root }", False),
])
def test_named_where_requires_predicate_declaration(pattern, host_registered):
    src = f"matcher m {{ produces FB\n {pattern}\n out {{ v: 1 }} }}"
    reg = Registries()
    if host_registered:
        reg.predicates["missing"] = lambda _block: True
    with pytest.raises(CompileError) as exc:
        _resolve(src, reg)
    diagnostic = next(
        d for d in exc.value.bag.items if d.code == "R044_UNDECLARED_PREDICATE"
    )
    assert "missing" in diagnostic.message
    assert "where missing" in src[diagnostic.span.start_off:diagnostic.span.end_off]


def test_named_where_forward_declaration():
    _resolve(
        "matcher m { produces FB\n op 1 @root where declared\n out { v: 1 } }\n"
        "predicate declared"
    )


@pytest.mark.parametrize("rule", [
    "rule leg = op 2 where missing",
    "rule leg = op 2 where missing -> maybe $leg",
])
def test_named_where_in_rule_requires_declaration(rule):
    with pytest.raises(CompileError) as exc:
        _resolve(rule)
    diagnostic = next(
        d for d in exc.value.bag.items if d.code == "R044_UNDECLARED_PREDICATE"
    )
    assert "missing" in diagnostic.message


def _resolve_expect_error(src: str, code: str, registries: Registries | None = None) -> None:
    with pytest.raises(CompileError) as exc:
        _resolve(src, registries)
    codes = [d.code for d in exc.value.bag.items]
    assert code in codes, f"expected diagnostic {code}, got {codes}"


def test_unknown_opcode_in_op_head():
    _resolve_expect_error(
        """
        rule r = op Missing @x
        """,
        "R001_UNKNOWN_OPCODE",
    )


def test_undefined_rule_reference():
    _resolve_expect_error(
        """
        opcode A = 0x1
        rule r = op A @a -> $no_such
        """,
        "R004_UNDEFINED_RULE",
    )


def test_undefined_rule_reference_in_matcher():
    src = "matcher m { produces FB\n op 1 @a -> $undefined\n out { v: 1 } }"
    with pytest.raises(CompileError) as exc:
        _resolve(src, _fb_registries())
    diagnostic, = exc.value.bag.items
    assert diagnostic.code == "R004_UNDEFINED_RULE"
    assert "'undefined'" in diagnostic.message
    assert src[diagnostic.span.start_off:diagnostic.span.end_off] == src


@pytest.mark.parametrize("kind, declaration", [
    ("rule", "rule r = op 1"),
    ("matcher", "matcher r { produces FB\n op 1 @a\n out { v: 1 } }"),
])
def test_duplicate_declaration_name_rejected(kind, declaration):
    src = declaration + "\n" + declaration
    with pytest.raises(CompileError) as exc:
        _resolve(src, _fb_registries())
    diagnostic, = exc.value.bag.items
    assert diagnostic.code == "R005_DUP_CAPTURE"
    assert diagnostic.message == f"duplicate {kind} 'r'"
    assert diagnostic.span.start_off == len(declaration) + 1


def test_duplicate_capture_in_same_matcher_branch():
    _resolve_expect_error(
        """
        opcode A = 0x1
        opcode B = 0x2
        matcher m {
            produces M

            op A @x -> op B @x
            out { v: 1 }
        }
        """,
        "R005_DUP_CAPTURE",
    )


def test_unbounded_recursion_rejected():
    _resolve_expect_error(
        """
        opcode A = 0x1
        rule r = op A @a -> $r
        """,
        "R006_UNBOUNDED_RECURSION",
    )


@pytest.mark.parametrize("head, capture", [
    ("op A @x", "x"),
    ("op A", ""),
    ("btype thing @x", "x"),
    ("btype thing", ""),
    ("pred p @x", "x"),
    ("pred p", ""),
])
def test_head_anchor_needs_no_entry(head, capture):
    reg = _m_registries()
    reg.predicates["p"] = lambda b: True
    rm = _resolve(
        f"opcode A = 1\nmatcher m {{ produces M\n{head}\nout {{ v: 1 }} }}",
        reg,
    ).matchers[0]
    assert rm.anchor_capture == capture
    assert list(rm.captures) == ([capture] if capture else [])
    assert rm.anchor_node.capture == (capture or None)
    assert rm.anchor_group is None


def test_uncaptured_head_stays_anchor_before_captured_child():
    rm = _resolve(
        "opcode A = 1\nopcode B = 2\n"
        "matcher m { produces M\nop A -> op B @child\nout { v: 1 } }",
        _m_registries(),
    ).matchers[0]
    assert rm.anchor_capture == ""
    assert rm.anchor_node.capture is None
    assert list(rm.captures) == ["child"]


def test_maybe_wrapped_head_rejected():
    _resolve_expect_error(
        """
        opcode A = 0x1
        matcher m {
            produces M

            maybe op A @x
            out { v: 1 }
        }
        """,
        "R007_ENTRY_VIOLATES_8_3",
    )


def test_peek_wrapped_head_rejected_with_head_reason():
    src = """
    opcode A = 0x1
    matcher m {
        produces M

        peek op A @x
        out { v: 1 }
    }
    """
    with pytest.raises(CompileError) as exc:
        _resolve(src, _m_registries())
    diagnostic = next(d for d in exc.value.bag.items if d.code == "R007_ENTRY_VIOLATES_8_3")
    assert diagnostic.message == "matcher 'm': head cannot be `peek`-wrapped"


def test_peek_wrapped_group_head_rejected_with_same_reason():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    matcher m {
        produces M

        peek xor { peek op A @x ; peek op B @x }
        out { v: 1 }
    }
    """
    with pytest.raises(CompileError) as exc:
        _resolve(src, _m_registries())
    diagnostic = next(d for d in exc.value.bag.items if d.code == "R007_ENTRY_VIOLATES_8_3")
    assert diagnostic.message == "matcher 'm': head cannot be `peek`-wrapped"


def _m_registries() -> Registries:
    return Registries()


def test_distinct_opcodes_with_different_captures_allowed():
    rf = _resolve(
        """
        opcode A = 0x1
        opcode B = 0x2
        matcher m {
            produces M

            op A @x -> op B @y
            out { v: 1 }
        }
        """,
        _m_registries(),
    )
    assert set(rf.matchers[0].captures.keys()) == {"x", "y"}


def test_same_name_captures_across_xor_branches_allowed():
    # Spec §6.2(a): the same capture name is legal across branches of an
    # exclusive alternative; only the winning branch binds.
    rf = _resolve(
        """
        opcode R = 0x1
        opcode A = 0x2
        opcode B = 0x3
        matcher m {
            produces M

            op R @r -> xor { op A @x ; op B @x }
            out { v: 1 }
        }
        """,
        _m_registries(),
    )
    caps = rf.matchers[0].captures
    assert set(caps.keys()) == {"r", "x"}
    # x appears in every branch of the XOR: it always binds.
    assert caps["x"].optional is False
    assert caps["x"].list_binding is False


def test_same_name_captures_across_eligible_or_allowed():
    # An `or` block over distinct concrete heads is exclusive by the upgrade heuristic.
    rf = _resolve(
        """
        opcode R = 0x1
        opcode A = 0x2
        opcode B = 0x3
        matcher m {
            produces M

            op R @r -> or { op A @x ; op B @x }
            out { v: 1 }
        }
        """,
        _m_registries(),
    )
    caps = rf.matchers[0].captures
    assert set(caps.keys()) == {"r", "x"}
    assert caps["x"].optional is False


def test_same_name_captures_partial_branch_coverage_is_optional():
    # x on 2 of 3 XOR branches: the third branch can win without binding x.
    rf = _resolve(
        """
        opcode R = 0x1
        opcode A = 0x2
        opcode B = 0x3
        opcode C = 0x4
        matcher m {
            produces M

            op R @r -> xor { op A @x ; op B @x ; op C }
            out { v: 1 }
        }
        """,
        _m_registries(),
    )
    assert rf.matchers[0].captures["x"].optional is True


def test_same_name_captures_across_non_exclusive_or_allowed():
    rf = _resolve(
        """
        opcode R = 0x1
        opcode A = 0x2
        opcode B = 0x3
        opcode C = 0x4
        matcher m {
            produces M

            op R @r -> or { op A @x -> op C ; op B @x }
            out { v: 1 }
        }
        """,
        _m_registries(),
    )
    caps = rf.matchers[0].captures
    assert set(caps.keys()) == {"r", "x"}
    assert caps["x"].head.ref == "A"
    assert caps["x"].optional is True
    assert caps["x"].list_binding is False


@pytest.mark.parametrize("pattern, head", [
    ("or { op A @x -> op Good ; op A @x -> op Other }", "A"),
    ("or { op A -> op B @x ; op C -> op D @x }", "B"),
    ("or { op A @x ; xor { op B @x ; op C @x } }", "A"),
])
def test_same_name_captures_across_any_alternative_allowed(pattern, head):
    rf = _resolve(
        "opcode R = 1\nopcode A = 2\nopcode B = 3\nopcode C = 4\n"
        "opcode D = 5\nopcode Good = 6\nopcode Other = 7\n"
        "matcher m { produces M\n"
        f"op R @r -> {pattern} \nout {{ v: 1 }} }}",
        _m_registries(),
    )
    caps = rf.matchers[0].captures
    assert set(caps.keys()) == {"r", "x"}
    assert caps["x"].head.ref == head
    assert caps["x"].head is caps["x"].source.head
    assert caps["x"].optional is True
    assert caps["x"].list_binding is False


@pytest.mark.parametrize("operator, optional", [("or", False), ("xor", True)])
def test_single_occurrence_capture_optionality_uses_exclusive_alternatives(operator, optional):
    rf = _resolve(
        "opcode R = 1\nopcode A = 2\nopcode B = 3\nopcode C = 4\n"
        "matcher m { produces M\n"
        f"op R @r -> {operator} {{ op A @x -> op B ; op C @y }} \nout {{ v: 1 }} }}",
        _m_registries(),
    )
    caps = rf.matchers[0].captures
    assert caps["x"].optional is optional
    assert caps["y"].optional is optional


@pytest.mark.parametrize("pattern", [
    "{ op A @x ; op B @x }",
    "or { op A @x -> op B @x ; op C }",
    "or { op A @x ; op B } -> op C @x",
])
def test_same_name_captures_without_separating_alternative_rejected(pattern):
    _resolve_expect_error(
        "opcode R = 1\nopcode A = 2\nopcode B = 3\nopcode C = 4\n"
        "matcher m { produces M\n"
        f"op R @r -> {pattern} \nout {{ v: 1 }} }}",
        "R005_DUP_CAPTURE", _m_registries(),
    )


def test_xor_group_head_accepted_as_opcode_set_anchor():
    rf = _resolve(
        """
        opcode A = 0x1
        opcode B = 0x2
        matcher m {
            produces M

            xor { op A @x ; op B @x }
            out { v: 1 }
        }
        """,
        _m_registries(),
    )
    rm = rf.matchers[0]
    assert rm.anchor_capture == "x"
    assert rm.anchor_group is not None
    assert len(rm.anchor_group.branches) == 2
    assert rm.captures["x"].optional is False


def test_heuristic_exclusive_or_group_head_accepted():
    # An `or` block over distinct concrete heads is a legal opcode-set anchor.
    rf = _resolve(
        """
        opcode A = 0x1
        opcode B = 0x2
        matcher m {
            produces M

            or { op A @x ; op B @x }
            out { v: 1 }
        }
        """,
        _m_registries(),
    )
    assert rf.matchers[0].anchor_group is not None


def test_non_exclusive_or_group_head_rejected():
    # Branch 2 is a chain, so the `or` block cannot anchor.
    _resolve_expect_error(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        matcher m {
            produces M

            or { op A @x ; (op B -> op C) }
            out { v: 1 }
        }
        """,
        "R007_ENTRY_VIOLATES_8_3",
    )


def test_maybe_wrapped_xor_group_head_rejected():
    _resolve_expect_error(
        """
        opcode A = 0x1
        opcode B = 0x2
        matcher m {
            produces M

            maybe xor { op A @x ; op B @x }
            out { v: 1 }
        }
        """,
        "R007_ENTRY_VIOLATES_8_3",
    )


def test_head_capture_missing_from_one_xor_branch_rejected():
    # @x covers only 2 of 3 branches: the C branch could win without binding
    # the anchor, so the group cannot anchor.
    _resolve_expect_error(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        matcher m {
            produces M

            xor { op A @x ; op B @x ; op C }
            out { v: 1 }
        }
        """,
        "R007_ENTRY_VIOLATES_8_3",
    )


def test_group_head_requires_same_capture_on_every_branch():
    _resolve_expect_error(
        "opcode A = 1\nopcode B = 2\n"
        "matcher m { produces M\nxor { op A @x ; op B @y }\nout { v: 1 } }",
        "R007_ENTRY_VIOLATES_8_3",
        _m_registries(),
    )


def test_xor_group_head_with_where_clause_accepted():
    # [M2R, R09]: a branch `where` clause is legal on a set-membership anchor
    # (was rejected under T10). Membership is tested first, then the matching
    # branch's predicate.
    reg = _m_registries()
    reg.predicates["p"] = lambda b: True
    rf = _resolve(
        """
        opcode A = 0x1
        opcode B = 0x2
        predicate p
        matcher m {
            produces M

            xor { op A @x ; op B @x where p }
            out { v: 1 }
        }
        """,
        reg,
    )
    rm = rf.matchers[0]
    assert rm.anchor_group is not None
    assert len(rm.anchor_group.branches) == 2
    # The where-carrying branch preserves its predicate for the anchor test.
    assert rm.anchor_group.branches[1].where_predicate == "p"


def test_mixed_op_btype_group_head_with_where_accepted():
    # [M2R, R09]: mixed op/btype heads AND a branch `where`, together.
    reg = _m_registries()
    reg.predicates["p"] = lambda b: True
    rf = _resolve(
        """
        opcode A = 0x1
        predicate p
        matcher m {
            produces M

            xor { op A @x where p ; btype jetton_transfer @x }
            out { v: 1 }
        }
        """,
        reg,
    )
    assert rf.matchers[0].anchor_group is not None


def test_pred_headed_node_accepted():
    reg = _m_registries()
    reg.predicates["is_special"] = lambda b: True
    rf = _resolve(
        """
        matcher m {
            produces M

            pred is_special @x
            out { v: 1 }
        }
        """,
        reg,
    )
    rm = rf.matchers[0]
    assert rm.anchor_capture == "x"
    assert rm.anchor_group is None


def test_group_head_branch_with_chain_rejected():
    # A branch that is a chain (not a bare node) cannot participate in an
    # opcode-set anchor even under explicit `xor`.
    _resolve_expect_error(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        matcher m {
            produces M

            xor { op A @x ; (op B @x -> op C) }
            out { v: 1 }
        }
        """,
        "R007_ENTRY_VIOLATES_8_3",
    )


def test_produces_union_needs_no_block_registry():
    rf = _resolve(
        """
        opcode ChangeDnsRecord = 0x1
        matcher change_dns_record {
            produces change_dns | renew_dns | delete_dns

            op ChangeDnsRecord @msg
            out { v: 1 }
        }
        """,
    )
    assert rf.matchers[0].decl.produces == ("change_dns", "renew_dns", "delete_dns")


def test_produces_union_dns_shaped_matcher_compiles_through_facade():
    # One anchor opcode and three possible produced types compile end to end.
    from indexer.events.mch import compile_text

    reg = Registries()

    src = """
    opcode ChangeDnsRecord = 0x1
    matcher change_dns_record {
        produces change_dns | renew_dns | delete_dns

        op ChangeDnsRecord @msg
        out { v: 1 }
    }
    """
    compiled = compile_text(src, "dns.mch", reg)
    assert len(compiled) == 1
    assert compiled[0].produces == ("change_dns", "renew_dns", "delete_dns")


def test_duplicate_opcode_captures_now_allowed():
    # Phase 1 (position-id capture extraction): two captures on the same opcode
    # are no longer rejected. The walker tells them apart by their position in
    # the lowered matcher tree.
    rf = _resolve(
        """
        opcode A = 0x1
        matcher m {
            produces M

            op A @first -> op A @second
            out { v: 1 }
        }
        """,
        _m_registries(),
    )
    assert set(rf.matchers[0].captures.keys()) == {"first", "second"}


def _error_codes(src: str, reg: Registries) -> list[str]:
    try:
        rf = _resolve(src, reg)
    except CompileError as ex:
        return [d.code for d in ex.bag.items]
    return []



def _parse_registries():
    reg = _fb_registries()
    reg.message_types["JettonTransfer"] = object  # stub parser class
    return reg


def test_r023_parse_composed_btype_capture_errors():
    # Parsing a composed/produced block (captured by `btype`) never round-trips:
    # its get_body() reads event_nodes[0], not the anchor protocol message.
    src = (
        "matcher m {\n produces FB\n"
        "  btype jetton_transfer @t\n"
        "  parse t as JettonTransfer\nout { v: 1 }\n}"
    )
    assert "R023_PARSE_TARGET_NOT_BASE_BLOCK" in _error_codes(src, _parse_registries())


def test_r023_parse_op_capture_ok():
    # Parsing a raw call-contract capture (`op`) is fine — get_body() is the
    # anchor's own body.
    src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n produces FB\n"
        "  op Foo @t\n  parse t as JettonTransfer\nout { v: 1 }\n}"
    )
    assert "R023_PARSE_TARGET_NOT_BASE_BLOCK" not in _error_codes(src, _parse_registries())



def test_comprehension_nested_rejected():
    # A comprehension inside another comprehension's body is R024.
    src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n produces FB\n"
        "  op Foo @x\n"
        "  let xs = [1, 2]\n"
        "  out { r: map(xs as s => any(xs as t => t > s)) }\n}"
    )
    assert "R024_NESTED_COMPREHENSION" in _error_codes(src, _fb_registries())


def test_comprehension_nested_in_xs_rejected():
    # A comprehension in the iterated list (`xs`) is R024 too.
    src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n produces FB\n"
        "  op Foo @x\n"
        "  let xs = [1, 2]\n"
        "  out { r: map(map(xs as s => s) as t => t) }\n}"
    )
    assert "R024_NESTED_COMPREHENSION" in _error_codes(src, _fb_registries())


def test_comprehension_binder_shadows_capture_ok():
    # The element var may shadow a `let`; body references resolve to the element,
    # and the shadow raises no R010/R013/R024.
    src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n produces FB\n"
        "  op Foo @x\n"
        "  let xs = [1, 2]\n"
        "  let s = 1\n"
        "  out { r: map(xs as s => s + 1) }\n}"
    )
    codes = _error_codes(src, _fb_registries())
    assert not any(c.startswith(("R010", "R013", "R024")) for c in codes), codes


def test_comprehension_body_unresolved_ref_rejected():
    # A body reference that is neither the element var nor an outer capture/let
    # is still R010.
    src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n produces FB\n"
        "  op Foo @x\n"
        "  let xs = [1, 2]\n"
        "  out { r: map(xs as s => s + zzz) }\n}"
    )
    assert "R010_UNRESOLVED_REFERENCE" in _error_codes(src, _fb_registries())


def test_comprehension_xs_unbound_var_not_visible_rejected():
    # The element var is NOT visible in `xs` (binds body only): referencing it in
    # `xs` is unresolved.
    src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n produces FB\n"
        "  op Foo @x\n"
        "  out { r: map(s as s => s) }\n}"
    )
    assert "R010_UNRESOLVED_REFERENCE" in _error_codes(src, _fb_registries())


@pytest.mark.parametrize("pattern, expanded", [
    ("op 1 @a { -> peek op 2 @x -> op 3 @y }",
     "op 1 @a -> peek op 2 @x -> op 3 @y"),
    ("op 1 @a { -> { op 2 @x ; maybe op 3 @y }; <- op 4 @p }",
     "op 1 @a -> { op 2 @x ; maybe op 3 @y } <- op 4 @p"),
    ("op 1 @a -> maybe peek op 2 @b { -> { op 3 @x ; maybe op 4 @y } }",
     "op 1 @a -> maybe peek (op 2 @b -> { op 3 @x ; maybe op 4 @y })"),
    ("op 1 @a { -> or { op 2 @x ; op 3 @x } }",
     "op 1 @a -> or { op 2 @x ; op 3 @x }"),
    ("op 1 @a -> op [2, 3] @b { -> op 4 @x }",
     "op 1 @a -> xor { op 2 @b -> op 4 @x ; op 3 @b -> op 4 @x }"),
])
def test_relation_capture_flags_match_arrows(pattern, expanded):
    captures = []
    for spelling in (pattern, expanded):
        rm = _resolve(
            "matcher m { produces FB\n" + spelling + "\n out { v: 1 } }",
            _fb_registries(),
        ).matchers[0]
        captures.append({
            name: (ci.optional, ci.peek, ci.list_binding)
            for name, ci in rm.captures.items()
        })
    assert captures[0] == captures[1]


@pytest.mark.parametrize("item", ["-> {}", "-> {{ {} }}", "<- {}"])
@pytest.mark.parametrize("scope", ["rule", "matcher"])
@pytest.mark.parametrize("target, code", [
    ("op Missing", "R001_UNKNOWN_OPCODE"),
])
def test_relation_targets_are_resolved(item, scope, target, code):
    pattern = f"op 1 @a {{ {item.format(target)} }}"
    src = ("rule r = " + pattern if scope == "rule" else
           "matcher m { produces FB\n" + pattern + "\n out { v: 1 } }")
    _resolve_expect_error(src, code, _fb_registries())


@pytest.mark.parametrize("item", ["-> {}", "-> {{ {} }}", "<- {}"])
def test_rule_references_inside_relations_are_classified(item):
    rf = _resolve(f"rule leg = op 1 {{ {item.format('maybe $leg')} }}")
    assert rf.rule_recursion["leg"] is RecursionClass.DIRECT_RECURSIVE
    _resolve_expect_error(
        f"rule leg = op 1 {{ {item.format('$missing')} }}", "R004_UNDEFINED_RULE",
    )


def test_relations_preserve_bare_node_requirement_for_head_groups():
    _resolve_expect_error(
        "matcher m { produces FB\n"
        "xor { op 1 @a { -> op 2 } ; op 3 @a }\n out { v: 1 } }",
        "R007_ENTRY_VIOLATES_8_3", _fb_registries(),
    )


@pytest.mark.parametrize("pattern, names", [
    ("op 1 @a as JettonTransfer -> op 2 @b as JettonTransfer", ["a", "b"]),
    ("op 1 @a -> or { op 2 @x -> op 3 @y as JettonTransfer ; op 4 @x as JettonTransfer }",
     ["x", "y"]),
    ("op 1 @a as JettonTransfer { <- op 4 @p as JettonTransfer; "
     "-> { op 3 @f as JettonTransfer; op 5 @g as JettonTransfer }; "
     "-> op 2 @b as JettonTransfer }", ["a", "b", "f", "g", "p"]),
])
def test_parse_annotation_prelude_uses_first_capture_order(pattern, names):
    from indexer.events.mch import ast_

    src = "matcher m { produces FB\n" + pattern + "\nlet v = 1\nout { v: v } }"
    rf = _resolve(src, _parse_registries())
    rm = rf.matchers[0]
    build_stmts = rm.decl.build_stmts
    prelude = build_stmts[:-2]
    assert [stmt.capture for stmt in prelude] == names
    assert all(stmt.msg_types == ("JettonTransfer",) for stmt in prelude)
    assert isinstance(build_stmts[-2], ast_.LetStmt)
    assert build_stmts[-2:] == rf.raw.matchers[0].build_stmts
    assert src[prelude[0].span.start_off:prelude[0].span.end_off].startswith("op ")


@pytest.mark.parametrize("pattern, written, code", [
    ("op 1 @a -> or { op 2 @x as T ; op 3 @x as U }", "", "R028_PARSE_ANNOTATION_CONFLICT"),
    ("op 1 @a -> or { op 2 @x as T | U ; op 3 @x as U | T }", "",
     "R028_PARSE_ANNOTATION_CONFLICT"),
])
def test_parse_annotation_errors(pattern, written, code):
    reg = _fb_registries()
    reg.message_types.update(T=object, U=object)
    _resolve_expect_error(
        "matcher m { produces FB\n" + pattern + "\n" + written + "\nout { v: 1 } }",
        code, reg,
    )


@pytest.mark.parametrize("head", ["btype thing", "pred p", "any"])
def test_parse_annotation_rejects_each_composed_occurrence(head):
    reg = _parse_registries()
    reg.predicates["p"] = lambda b: True
    _resolve_expect_error(
        "matcher m { produces FB\n"
        f"op 1 @a -> or {{ op 2 @x as JettonTransfer ; {head} @x as JettonTransfer }}\n"
        "out { v: 1 } }",
        "R030_PARSE_ANNOTATION_ON_COMPOSED", reg,
    )


@pytest.mark.parametrize("src, code", [
    ("fn f(x) = x + anchor", "R031_FN_BODY_FREE_NAME"),
    ("fn f(x) = {v: [x, missing.value]}", "R031_FN_BODY_FREE_NAME"),
    ("fn f(x) = map(x as e => e)", "R033_FN_BODY_COMPREHENSION"),
    ("fn len(x) = x", "R035_FN_NAME_COLLISION"),
    ("fn account(x) = x", "R035_FN_NAME_COLLISION"),
    ("fn f(x) = x\nfn g() = f()", "R036_FN_ARITY"),
    ("fn f(x) = x\nfn g() = f(1, 2)", "R036_FN_ARITY"),
])
def test_fn_errors(src, code):
    with pytest.raises(CompileError) as exc:
        _resolve(src)
    assert [d.code for d in exc.value.bag.items] == [code]


def test_fn_registered_host_name_collision():
    reg = Registries()

    @reg.register_fn("host_fn")
    async def host_fn(x):
        return x

    with pytest.raises(CompileError) as exc:
        _resolve("fn host_fn(x) = x", reg)
    assert [d.code for d in exc.value.bag.items] == ["R035_FN_NAME_COLLISION"]


@pytest.mark.parametrize("statements", [
    "let v = f(ARG)\nout { v: v }",
    "require v = f(ARG)\nout { v: v }",
    "reject when f(ARG)\nout { v: 1 }",
    "failed when f(ARG)\nout { v: 1 }",
    "out { v: {items: [f(ARG)]} }",
    "produces switch { when f(ARG) => FB out { v: 1 } else => FB out { v: 2 } }",
    "produces switch { when true => FB out { v: f(ARG) } else => FB out { v: 2 } }",
    "produces switch { when true => FB out { v: 1 } else => FB out { v: f(ARG) } }",
    "out { v: map(f(ARG) as e => e) }",
    "out { v: map([1] as e => f(ARG)) }",
])
@pytest.mark.parametrize("arg, code", [("", "R036_FN_ARITY"), ("missing", "R010_UNRESOLVED_REFERENCE")])
def test_fn_expands_before_build_validation(statements, arg, code):
    src = (
        "fn f(x) = x\nmatcher m { produces FB\n op 1 @a\n"
        + statements.replace("ARG", arg) + "\n}"
    )
    with pytest.raises(CompileError) as exc:
        _resolve(src, _fb_registries())
    assert [d.code for d in exc.value.bag.items] == [code]


def test_fn_substitution_reuses_arguments_and_preserves_spans():
    from indexer.events.mch import ast_

    src = (
        "fn twice(x) = x + x\n"
        "matcher m { produces FB\n op 1 @a\n"
        "let v = twice(a.msg.value)\nout { v: v } }"
    )
    file = parse(tokenize(src, "test.mch"), "test.mch")
    call = file.matchers[0].build_stmts[0].value
    resolved = resolve(file, _fb_registries())
    expr = resolved.matchers[0].decl.build_stmts[0].value
    assert isinstance(expr, ast_.BinaryOp)
    assert expr.left is expr.right
    assert expr.left.span == call.args[0].span
    assert src[expr.left.span.start_off:expr.left.span.end_off] == "a.msg.value"
    assert file.matchers[0].build_stmts[0].value is call


def test_fn_preserves_comprehension_scope():
    rf = _resolve(
        "fn f(x) = x + 1\nmatcher m { produces FB\n op 1 @a\n"
        "out { v: map([1] as a => f(a)) } }", _fb_registries(),
    )
    _resolve_expect_error(
        "fn f(x) = x\nmatcher m { produces FB\n op 1 @a\n"
        "out { v: map(f(e) as e => e) } }", "R010_UNRESOLVED_REFERENCE", _fb_registries(),
    )
    _resolve_expect_error(
        "fn f(x) = x\nmatcher m { produces FB\n op 1 @a\n"
        "out { v: map([1] as e => f(map([2] as v => v))) } }",
        "R024_NESTED_COMPREHENSION", _fb_registries(),
    )
