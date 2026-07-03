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


def test_entry_on_xor_branch_with_where_clause_rejected():
    # `where` on an opcode-set anchor branch is rejected cleanly rather than
    # silently dropped (there is no single anchor node to wrap test_self on).
    reg = _m_registries()
    reg.predicates["p"] = lambda b: True
    _resolve_expect_error(
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
        "R007_ENTRY_VIOLATES_8_3",
        registries=reg,
    )


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
