from __future__ import annotations

import pytest

from indexer.events.mch.diagnostics import CompileError, Severity
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


# --- priority directive (§8.6, T20) ----------------------------------------

def _two_matchers(p_a: str, p_c: str) -> str:
    return (
        "opcode Foo = 0xF00\n"
        f"matcher a {{ produces FB\n build b\n entry @x\n{p_a} op Foo @x }}\n"
        f"matcher c {{ produces FB\n build d\n entry @y\n{p_c} op Foo @y }}\n"
    )


def _warn_codes(src: str) -> list[str]:
    return [d.code for d in _resolve(src, _fb_registries()).diagnostics.items]


def test_priority_default_is_100():
    rf = _resolve("opcode Foo = 0xF00\nmatcher a { produces FB\n build b\n entry @x\n op Foo @x }",
                  _fb_registries())
    assert rf.raw.matchers[0].priority == 100


def test_priority_parsed_value():
    rf = _resolve("opcode Foo = 0xF00\nmatcher a { produces FB\n build b\n entry @x\n priority 42\n op Foo @x }",
                  _fb_registries())
    assert rf.raw.matchers[0].priority == 42


def test_priority_duplicate_nondefault_warns():
    assert "W010_DUPLICATE_PRIORITY" in _warn_codes(_two_matchers("priority 50\n", "priority 50\n"))


def test_priority_distinct_no_warning():
    assert "W010_DUPLICATE_PRIORITY" not in _warn_codes(_two_matchers("priority 50\n", "priority 60\n"))


def test_priority_default_pair_no_warning():
    # Two default-priority matchers are the norm and must NOT warn.
    assert "W010_DUPLICATE_PRIORITY" not in _warn_codes(_two_matchers("", ""))


def test_resolve_opcode_value_matches_host():
    reg = Registries()
    reg.opcodes["JettonTransfer"] = 0x0f8a7ea5
    rf = _resolve("opcode JettonTransfer = 0x0f8a7ea5", reg)
    assert rf.opcodes["JettonTransfer"] == 0x0f8a7ea5


def _fb_registries() -> Registries:
    reg = Registries()
    reg.block_types["FB"] = object
    return reg


def test_resolve_matcher_captures_inferred():
    src = """
    opcode Foo = 0x1
    opcode Bar = 0x2
    matcher m {
        produces FB
        build    build_m
        entry    @a

        op Foo @a -> op Bar @b
    }
    """
    rf = _resolve(src, _fb_registries())
    m = rf.matchers[0]
    assert set(m.captures.keys()) == {"a", "b"}
    ci_a = m.captures["a"]
    assert ci_a.optional is False
    assert ci_a.list_binding is False


def test_resolve_maybe_marks_capture_optional():
    src = """
    opcode Foo = 0x1
    opcode Bar = 0x2
    matcher m {
        produces FB
        build    build_m
        entry    @a

        op Foo @a -> maybe op Bar @b
    }
    """
    rf = _resolve(src, _fb_registries())
    m = rf.matchers[0]
    assert m.captures["a"].optional is False
    assert m.captures["b"].optional is True


def test_resolve_anchor_resolved():
    src = """
    opcode Foo = 0x1
    matcher m {
        produces FB
        build    build_m
        entry    @a

        op Foo @a
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


def _resolve_expect_error(src: str, code: str, registries: Registries | None = None) -> None:
    with pytest.raises(CompileError) as exc:
        _resolve(src, registries)
    codes = [d.code for d in exc.value.bag.items if d.severity is Severity.ERROR]
    assert code in codes, f"expected diagnostic {code}, got {codes}"


def test_unknown_opcode_in_op_head():
    _resolve_expect_error(
        """
        rule r = op Missing @x
        """,
        "R001_UNKNOWN_OPCODE",
    )


def test_opcode_value_mismatch_with_host():
    reg = Registries()
    reg.opcodes["JettonTransfer"] = 0xdead
    _resolve_expect_error(
        "opcode JettonTransfer = 0x0f8a7ea5",
        "R002_OPCODE_VALUE_MISMATCH",
        registries=reg,
    )


def test_unknown_predicate_in_where_clause():
    _resolve_expect_error(
        """
        opcode A = 0x1
        rule r = op A @a where missing_pred
        """,
        "R003_UNKNOWN_PREDICATE",
    )


def test_undefined_rule_reference():
    _resolve_expect_error(
        """
        opcode A = 0x1
        rule r = op A @a -> $no_such
        """,
        "R004_UNDEFINED_RULE",
    )


def test_duplicate_capture_in_same_matcher_branch():
    _resolve_expect_error(
        """
        opcode A = 0x1
        opcode B = 0x2
        matcher m {
            produces M
            build    build_m
            entry    @x

            op A @x -> op B @x
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


def test_entry_required():
    _resolve_expect_error(
        """
        opcode A = 0x1
        matcher m {
            produces M
            build    build_m

            op A @x
        }
        """,
        "R008_ENTRY_REQUIRED",
    )


def test_entry_inside_maybe_rejected():
    _resolve_expect_error(
        """
        opcode A = 0x1
        matcher m {
            produces M
            build    build_m
            entry    @x

            maybe op A @x
        }
        """,
        "R007_ENTRY_VIOLATES_8_3",
    )


def _m_registries() -> Registries:
    reg = Registries()
    reg.block_types["M"] = object
    return reg


def test_distinct_opcodes_with_different_captures_allowed():
    rf = _resolve(
        """
        opcode A = 0x1
        opcode B = 0x2
        matcher m {
            produces M
            build    build_m
            entry    @x

            op A @x -> op B @y
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
            build    build_m
            entry    @r

            op R @r -> (op A @x ^ op B @x)
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
    # `|` alternative whose branches are bare concrete nodes with distinct heads
    # is exclusive by the upgrade heuristic — same-name captures allowed.
    rf = _resolve(
        """
        opcode R = 0x1
        opcode A = 0x2
        opcode B = 0x3
        matcher m {
            produces M
            build    build_m
            entry    @r

            op R @r -> (op A @x | op B @x)
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
            build    build_m
            entry    @r

            op R @r -> (op A @x ^ op B @x ^ op C)
        }
        """,
        _m_registries(),
    )
    assert rf.matchers[0].captures["x"].optional is True


def test_same_name_captures_across_non_exclusive_or_rejected():
    # First branch is a chain, not a bare node — the OR is not provably
    # exclusive, so the duplicate name is still an error.
    _resolve_expect_error(
        """
        opcode R = 0x1
        opcode A = 0x2
        opcode B = 0x3
        opcode C = 0x4
        matcher m {
            produces M
            build    build_m
            entry    @r

            op R @r -> (op A @x -> op C | op B @x)
        }
        """,
        "R005_DUP_CAPTURE",
    )


def test_entry_on_xor_group_accepted_as_opcode_set_anchor():
    # Spec §8.3 [M2] (T10): entry on a capture bound on every branch of an XOR
    # of bare concrete heads is an opcode-set anchor.
    rf = _resolve(
        """
        opcode A = 0x1
        opcode B = 0x2
        matcher m {
            produces M
            build    build_m
            entry    @x

            (op A @x ^ op B @x)
        }
        """,
        _m_registries(),
    )
    rm = rf.matchers[0]
    assert rm.anchor_capture == "x"
    assert rm.anchor_group is not None
    assert len(rm.anchor_group.branches) == 2
    assert rm.captures["x"].optional is False


def test_entry_on_heuristic_exclusive_or_group_accepted():
    # `|` alternative of distinct bare concrete heads meets the ExclusiveOr
    # upgrade heuristic — also a legal opcode-set anchor.
    rf = _resolve(
        """
        opcode A = 0x1
        opcode B = 0x2
        matcher m {
            produces M
            build    build_m
            entry    @x

            (op A @x | op B @x)
        }
        """,
        _m_registries(),
    )
    assert rf.matchers[0].anchor_group is not None


def test_entry_on_non_exclusive_or_alternative_rejected():
    # Branch 2 is a chain, so the `|` alternative is not provably exclusive —
    # its captures can never anchor.
    _resolve_expect_error(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        matcher m {
            produces M
            build    build_m
            entry    @x

            (op A @x | (op B -> op C))
        }
        """,
        "R007_ENTRY_VIOLATES_8_3",
    )


def test_entry_on_maybe_wrapped_xor_group_rejected():
    _resolve_expect_error(
        """
        opcode A = 0x1
        opcode B = 0x2
        matcher m {
            produces M
            build    build_m
            entry    @x

            maybe (op A @x ^ op B @x)
        }
        """,
        "R007_ENTRY_VIOLATES_8_3",
    )


def test_entry_capture_missing_from_one_xor_branch_rejected():
    # @x covers only 2 of 3 branches: the C branch could win without binding
    # the anchor, so the group cannot anchor.
    _resolve_expect_error(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        matcher m {
            produces M
            build    build_m
            entry    @x

            (op A @x ^ op B @x ^ op C)
        }
        """,
        "R007_ENTRY_VIOLATES_8_3",
    )


def test_entry_on_xor_branch_with_where_clause_accepted():
    # [M2R, R09]: a branch `where` clause is legal on a set-membership anchor
    # (was rejected under T10). Membership is tested first, then the matching
    # branch's predicate.
    reg = _m_registries()
    reg.predicates["p"] = lambda b: True
    rf = _resolve(
        """
        opcode A = 0x1
        opcode B = 0x2
        matcher m {
            produces M
            build    build_m
            entry    @x

            (op A @x ^ op B @x where p)
        }
        """,
        reg,
    )
    rm = rf.matchers[0]
    assert rm.anchor_group is not None
    assert len(rm.anchor_group.branches) == 2
    # The where-carrying branch preserves its predicate for the anchor test.
    assert rm.anchor_group.branches[1].where_predicate == "p"


def test_entry_on_mixed_op_btype_xor_with_where_accepted():
    # [M2R, R09]: mixed op/btype heads AND a branch `where`, together.
    reg = _m_registries()
    reg.predicates["p"] = lambda b: True
    rf = _resolve(
        """
        opcode A = 0x1
        matcher m {
            produces M
            build    build_m
            entry    @x

            (op A @x where p ^ btype jetton_transfer @x)
        }
        """,
        reg,
    )
    assert rf.matchers[0].anchor_group is not None


def test_entry_on_pred_headed_node_accepted():
    # [M2R, R10]: `entry` may reference a `pred`-headed node (no opcode/btype
    # prefilter; the engine full-scans test_self). Was rejected under M2.
    reg = _m_registries()
    reg.predicates["is_special"] = lambda b: True
    rf = _resolve(
        """
        matcher m {
            produces M
            build    build_m
            entry    @x

            pred is_special @x
        }
        """,
        reg,
    )
    rm = rf.matchers[0]
    assert rm.anchor_capture == "x"
    assert rm.anchor_group is None


def test_entry_on_xor_branch_with_chain_rejected():
    # A branch that is a chain (not a bare node) cannot participate in an
    # opcode-set anchor even under explicit `^`.
    _resolve_expect_error(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        matcher m {
            produces M
            build    build_m
            entry    @x

            (op A @x ^ (op B @x -> op C))
        }
        """,
        "R007_ENTRY_VIOLATES_8_3",
    )


def test_produces_union_unknown_name_rejected():
    # spec §8.1 M2: every name in a `produces` union is validated against the
    # host block-type registry, not just the first.
    reg = Registries()
    reg.block_types["change_dns"] = object
    reg.block_types["renew_dns"] = object
    # delete_dns intentionally left unregistered.
    _resolve_expect_error(
        """
        opcode ChangeDnsRecord = 0x1
        matcher change_dns_record {
            produces change_dns | renew_dns | delete_dns
            build    build_dns
            entry    @msg

            op ChangeDnsRecord @msg
        }
        """,
        "C003_UNKNOWN_BLOCK_TYPE",
        registries=reg,
    )


def test_produces_union_dns_shaped_matcher_compiles_through_facade():
    # spec §8.1 / §14.5 (pattern part): one anchor opcode, three possible
    # produced types, all registered — compiles end to end through resolve + compile.
    from indexer.events.mch import compile_text

    reg = Registries()
    reg.block_types["change_dns"] = object
    reg.block_types["renew_dns"] = object
    reg.block_types["delete_dns"] = object

    @reg.register_builder("build_dns")
    async def _b(m):  # noqa: ARG001
        return None

    src = """
    opcode ChangeDnsRecord = 0x1
    matcher change_dns_record {
        produces change_dns | renew_dns | delete_dns
        build    build_dns
        entry    @msg

        op ChangeDnsRecord @msg
    }
    """
    compiled = compile_text(src, "dns.mch", reg)
    assert len(compiled) == 1
    assert compiled[0].produces_cls_name == "change_dns"


def test_duplicate_opcode_captures_now_allowed():
    # Phase 1 (position-id capture extraction): two captures on the same opcode
    # are no longer rejected. The walker tells them apart by their position in
    # the lowered matcher tree.
    rf = _resolve(
        """
        opcode A = 0x1
        matcher m {
            produces M
            build    build_m
            entry    @first

            op A @first -> op A @second
        }
        """,
        _m_registries(),
    )
    assert set(rf.matchers[0].captures.keys()) == {"first", "second"}


# --- F3: produces class -> btype normalization (R022) -----------------------

def _error_codes(src: str, reg: Registries) -> list[str]:
    try:
        rf = _resolve(src, reg)
    except CompileError as ex:
        return [d.code for d in ex.bag.items]
    return [d.code for d in rf.diagnostics.items]


def _real_block_registries():
    """Registries whose block_types map a class name to a REAL typed Block
    subclass (btype != class name), exercising the R022 registry-aware check."""
    from indexer.events.blocks.core import Block

    class FakeThingBlock(Block):
        def __init__(self):
            super().__init__("fake_thing", [], None)

    reg = Registries()
    reg.block_types["FakeThingBlock"] = FakeThingBlock
    reg.block_types["fake_thing"] = FakeThingBlock
    reg.block_types["JettonTransferBlock"] = FakeThingBlock  # mapped name, any class
    return reg


def test_r022_declarative_unmapped_class_errors():
    # A declarative matcher (no `build`) producing a real typed class absent from
    # CLASS_TO_BTYPE would silently emit the class-name string as the btype.
    src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n produces FakeThingBlock\n entry @x\n op Foo @x\n"
        "  out { a: 1 }\n}"
    )
    assert "R022_UNNORMALIZED_PRODUCES_CLASS" in _error_codes(src, _real_block_registries())


def test_r022_bare_btype_ok():
    # The snake_case bare-btype form resolves by identity; no diagnostic.
    src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n produces fake_thing\n entry @x\n op Foo @x\n"
        "  out { a: 1 }\n}"
    )
    assert "R022_UNNORMALIZED_PRODUCES_CLASS" not in _error_codes(src, _real_block_registries())


def test_r022_mapped_class_ok():
    # A class name present in CLASS_TO_BTYPE resolves; no diagnostic.
    src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n produces JettonTransferBlock\n entry @x\n op Foo @x\n"
        "  out { a: 1 }\n}"
    )
    assert "R022_UNNORMALIZED_PRODUCES_CLASS" not in _error_codes(src, _real_block_registries())


def test_r022_builder_matcher_exempt():
    # A builder matcher constructs its own block, so an unmapped `produces` class
    # is documentation only — no R022 (identity fallback is harmless).
    reg = _real_block_registries()

    async def _b(match):
        return None
    reg.builders["build_m"] = _b
    src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n produces FakeThingBlock\n build build_m\n entry @x\n op Foo @x\n}"
    )
    assert "R022_UNNORMALIZED_PRODUCES_CLASS" not in _error_codes(src, reg)


def test_r022_stub_block_type_exempt():
    # A stub-registered produces (block_types[name] is `object`) keeps identity;
    # this is the synthetic-test-matcher shape and must not trip R022.
    src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n produces FB\n entry @x\n op Foo @x\n  out { a: 1 }\n}"
    )
    assert "R022_UNNORMALIZED_PRODUCES_CLASS" not in _error_codes(src, _fb_registries())


# --- F1: parse on a composed btype capture (R023) ---------------------------

def _parse_registries():
    reg = _fb_registries()
    reg.message_types["JettonTransfer"] = object  # stub parser class
    return reg


def test_r023_parse_composed_btype_capture_errors():
    # Parsing a composed/produced block (captured by `btype`) never round-trips:
    # its get_body() reads event_nodes[0], not the anchor protocol message.
    src = (
        "matcher m {\n produces FB\n build b\n entry @t\n"
        "  btype jetton_transfer @t\n"
        "  parse t as JettonTransfer\n}"
    )
    assert "R023_PARSE_TARGET_COMPOSED_BLOCK" in _error_codes(src, _parse_registries())


def test_r023_parse_op_capture_ok():
    # Parsing a raw call-contract capture (`op`) is fine — get_body() is the
    # anchor's own body.
    src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n produces FB\n build b\n entry @t\n"
        "  op Foo @t\n  parse t as JettonTransfer\n}"
    )
    assert "R023_PARSE_TARGET_COMPOSED_BLOCK" not in _error_codes(src, _parse_registries())


# --- G-LC/G-Q comprehensions (§16.1) ---------------------------------------

def test_comprehension_nested_rejected():
    # A comprehension inside another comprehension's body is R024.
    src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n produces FB\n entry @x\n"
        "  op Foo @x\n"
        "  let xs = [1, 2]\n"
        "  out { r: map(xs as s => any(xs as t => t > s)) }\n}"
    )
    assert "R024_NESTED_COMPREHENSION" in _error_codes(src, _fb_registries())


def test_comprehension_nested_in_xs_rejected():
    # A comprehension in the iterated list (`xs`) is R024 too.
    src = (
        "opcode Foo = 0x1\n"
        "matcher m {\n produces FB\n entry @x\n"
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
        "matcher m {\n produces FB\n entry @x\n"
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
        "matcher m {\n produces FB\n entry @x\n"
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
        "matcher m {\n produces FB\n entry @x\n"
        "  op Foo @x\n"
        "  out { r: map(s as s => s) }\n}"
    )
    assert "R010_UNRESOLVED_REFERENCE" in _error_codes(src, _fb_registries())
