"""Structural lowering conformance for emitted IR JSON and compile diagnostics.

These tests compile matcher specs, inspect emitted node and anchor records, and
check frontend rejections. Runtime behavior is covered by engine CTest vector
corpora, walker cases, and action goldens.
"""
from __future__ import annotations

import pytest

from indexer.events.mch.compile import compile_text
from indexer.events.mch.diagnostics import CompileError
from indexer.events.mch.ir_emit import emit_ir
from indexer.events.mch.registry import Registries


def _registries_with_opcodes(**opcodes: int) -> Registries:
    r = Registries()
    r.opcodes.update(opcodes)
    return r


def _emit_one(src: str, **opcodes: int):
    """Compile one matcher and return (matcher_record, nodes) from its IR."""
    reg = _registries_with_opcodes(**opcodes)
    art = emit_ir(compile_text(src, "test.mch", reg))
    return art["matchers"][0], art["nodes"]


def test_missing_output_rejected_through_compile_facade():
    src = """
    opcode Foo = 0x1
    matcher m {
        produces MB
        op Foo @x
    }
    """
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", _registries_with_opcodes(Foo=1))
    assert "R016_BUILD_NO_OUT" in [d.code for d in exc.value.bag.items]


def test_empty_out_is_a_build_program():
    src = """
    opcode Foo = 0x1
    matcher m {
        produces MB
        op Foo @x
        out {}
    }
    """
    matcher, _ = _emit_one(src, Foo=1)
    assert matcher["build_program"] == [{"s": "out", "fields": []}]


# ==========================================================================
# Atoms and single-edge chains
# ==========================================================================

def test_single_op_node_emits_contract_root_and_opcode_set_anchor():
    src = """
    opcode JettonTransfer = 0x0f8a7ea5

    matcher jetton_transfer {
        produces JettonTransferBlock

        op JettonTransfer @t
        out { v: 1 }
    }
    """
    reg = _registries_with_opcodes(JettonTransfer=0x0F8A7EA5)
    art = emit_ir(compile_text(src, "test.mch", reg))
    m, nodes = art["matchers"][0], art["nodes"]

    assert m["name"] == "jetton_transfer"
    assert m["anchor"] == {"kind": "opcode_set", "values": [0x0F8A7EA5]}
    root = nodes[m["root"]]
    assert root["kind"] == "contract"
    assert root["opcode"] == 0x0F8A7EA5
    assert root["capture"] == "t"


@pytest.mark.parametrize(
    "case, pattern",
    [
        ("root", "op Zero @a"),
        ("child", "op One @a -> op Zero"),
        ("explicit_xor", "op One @a -> xor { op Zero; op 0 }"),
        ("automatic_exclusivity", "op One @a -> or { op Zero; op Nine }"),
        ("homogeneous_anchor", "xor { op Zero @a; op Nine @a }"),
        ("mixed_anchor", "xor { op Zero @a; btype MB @a }"),
        ("literal", "op 0 @a"),
    ],
)
def test_declared_zero_opcode_resolves_everywhere(case, pattern):
    src = f"""
    opcode Zero = 0
    opcode One = 1
    opcode Nine = 9
    matcher m {{ produces MB
        {pattern}
        out {{ v: 1 }}
    }}
    """
    if case == "explicit_xor":
        with pytest.raises(CompileError) as exc:
            _emit_one(src, Zero=9, One=1, Nine=9)
        assert "W202_EXCLUSIVE_OR_REDUNDANT_HEADS" in [d.code for d in exc.value.bag.items]
        return

    matcher, nodes = _emit_one(src, Zero=9, One=1, Nine=9)
    root = nodes[matcher["root"]]
    if case in {"root", "literal"}:
        assert root["opcode"] == 0
    elif case == "child":
        assert nodes[root["child"]]["opcode"] == 0
    elif case == "automatic_exclusivity":
        choice = nodes[root["child"]]
        assert choice["kind"] == "or" and choice["exclusive"] is True
    elif case == "homogeneous_anchor":
        assert matcher["anchor"] == {"kind": "opcode_set", "values": [0, 9]}
    else:
        assert matcher["anchor"] == {
            "kind": "mixed",
            "branches": [{"op": 0}, {"btype": "MB"}],
        }


def test_ungrouped_head_preserves_full_chain():
    m, nodes = _emit_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        matcher m {
            produces MB
            op A @a -> op B @b -> op C @c
            out { v: 1 }
        }
        """,
        A=0x1, B=0x2, C=0x3,
    )
    a = nodes[m["root"]]
    assert a["kind"] == "contract" and a["opcode"] == 0x1
    b = nodes[a["child"]]
    assert b["kind"] == "contract" and b["opcode"] == 0x2
    c = nodes[b["child"]]
    assert c["kind"] == "contract" and c["opcode"] == 0x3
    assert "child" not in c


def test_maybe_wrapped_child_emits_optional():
    m, nodes = _emit_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        matcher m {
            produces MB
            op A @a -> maybe op B @b
            out { v: 1 }
        }
        """,
        A=0x1, B=0x2,
    )
    child = nodes[nodes[m["root"]]["child"]]
    assert child["optional"] is True


def test_alternative_emits_or_node():
    m, nodes = _emit_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        matcher m {
            produces MB
            op A @a -> or { op B @b ; op C @c }
            out { v: 1 }
        }
        """,
        A=0x1, B=0x2, C=0x3,
    )
    child = nodes[nodes[m["root"]]["child"]]
    assert child["kind"] == "or"


def test_btype_node_emits_block_type():
    m, nodes = _emit_one(
        """
        opcode A = 0x1
        matcher m {
            produces MB
            op A @a -> btype jetton_transfer @j
            out { v: 1 }
        }
        """,
        A=0x1,
    )
    child = nodes[nodes[m["root"]]["child"]]
    assert child["kind"] == "block_type"
    assert child["btype"] == "jetton_transfer"


def test_btype_anchor_emits_btype_anchor_and_block_type_root():
    m, nodes = _emit_one(
        """
        matcher m {
            produces MB
            btype jetton_transfer @j
            out { v: 1 }
        }
        """,
    )
    assert m["anchor"] == {"kind": "btype", "values": ["jetton_transfer"]}
    root = nodes[m["root"]]
    assert root["kind"] == "block_type"
    assert root["btype"] == "jetton_transfer"


def test_children_block_emits_children_list():
    m, nodes = _emit_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        matcher m {
            produces MB
            op A @a -> {
                op B @b
                op C @c
            }
            out { v: 1 }
        }
        """,
        A=0x1, B=0x2, C=0x3,
    )
    root = nodes[m["root"]]
    assert root["kind"] == "contract" and root["opcode"] == 0x1
    assert [nodes[i]["opcode"] for i in root["children"]] == [0x2, 0x3]
    assert "child" not in root


def test_recursive_rule_emits_recursive_node():
    m, nodes = _emit_one(
        """
        opcode A      = 0x1
        opcode B      = 0x2
        opcode Anchor = 0x3

        rule swap_step =
            or { op A @step -> maybe $swap_step ; op B @term }

        matcher m {
            produces MB

            op Anchor @anchor -> $swap_step
            out { v: 1 }
        }
        """,
        A=0x1, B=0x2, Anchor=0x3,
    )
    assert any(n["kind"] == "recursive" for n in nodes), "expected a recursive node in the graph"


@pytest.mark.parametrize(
    "head",
    [
        "or { op 2; op 3 }",
        "xor { op 2; op 3 }",
        "$r",
        "maybe or { op 2; op 3 }",
        "peek xor { op 2; op 3 }",
        "maybe $r",
    ],
)
@pytest.mark.parametrize("tail", ["-> op 4", "<- op 4", "-> { op 4 }"])
def test_chain_tail_after_choice_or_recursive_ref_rejected(head, tail):
    src = f"""
    rule r = or {{ op 2 -> maybe $r; op 3 }}
    matcher m {{ produces MB
        op 1 @a -> {head} {tail}
        out {{ v: 1 }}
    }}
    """
    artifact = None
    with pytest.raises(CompileError) as exc:
        artifact = emit_ir(compile_text(src, "test.mch", Registries()))
    assert artifact is None
    assert "C009_MIXED_CHILD_PARENT_CHAIN_UNSUPPORTED" in [d.code for d in exc.value.bag.items]


@pytest.mark.parametrize(
    "rules, pattern",
    [
        ("", "op 1 @a -> (op 2 -> or { op 3; op 4 }) -> op 5"),
        ("", "op 1 @a <- (op 2 <- or { op 3; op 4 }) <- op 5"),
        (
            "rule choice = op 2 -> or { op 3; op 4 }",
            "op 1 @a -> $choice -> op 5",
        ),
    ],
)
def test_chain_tail_rejection_checks_actual_attachment_node(rules, pattern):
    src = f"""
    {rules}
    matcher m {{ produces MB
        {pattern}
        out {{ v: 1 }}
    }}
    """
    assert "C009_MIXED_CHILD_PARENT_CHAIN_UNSUPPORTED" in _compile_error_codes(src)


def test_chain_tail_after_nonrecursive_leaf_ref_is_preserved():
    matcher, nodes = _emit_one("""
        rule leaf = op 2
        matcher m { produces MB
            op 1 @a -> $leaf -> op 5
            out { v: 1 }
        }
    """)
    root = nodes[matcher["root"]]
    leaf = nodes[root["child"]]
    assert leaf["opcode"] == 2
    assert nodes[leaf["child"]]["opcode"] == 5


def test_recursive_rule_with_alternative_tail_rejected():
    src = """
    rule r = op A @step -> or { maybe $r; op B @term }
    matcher m { produces MB
        op C @anchor -> $r
        out { v: 1 }
    }
    """
    with pytest.raises(CompileError) as exc:
        _emit_one(src, A=1, B=2, C=3)
    diagnostic, = exc.value.bag.items
    assert diagnostic.code == "C001_RECURSION_SHAPE_UNSUPPORTED"
    assert src[diagnostic.span.start_off:diagnostic.span.end_off].startswith("rule r")


def test_multi_hop_parent_chain_emits_parent_chain():
    """`op A @a <- op B <- op C` lowers to a chain of parent edges; leaf is C."""
    m, nodes = _emit_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        matcher m {
            produces MB
            op A @a <- op B @b <- op C @c
            out { v: 1 }
        }
        """,
        A=0x1, B=0x2, C=0x3,
    )
    a = nodes[m["root"]]
    assert "child" not in a and "children" not in a
    b = nodes[a["parent"]]
    assert b["kind"] == "contract" and b["opcode"] == 0x2
    c = nodes[b["parent"]]
    assert c["kind"] == "contract" and c["opcode"] == 0x3
    assert "parent" not in c


# ==========================================================================
# XOR block: exclusive flag on the emitted `or` node
# ==========================================================================

def _or_child(src: str, **opcodes: int) -> dict:
    m, nodes = _emit_one(src, **opcodes)
    return nodes[nodes[m["root"]]["child"]]


def test_xor_emits_exclusive_or():
    child = _or_child(
        """
        opcode X = 0x9
        opcode A = 0x1
        opcode B = 0x2
        matcher m { produces MB
            op X @x -> xor { op A @a ; op B @b }
            out { v: 1 }
        }
        """,
        X=0x9, A=0x1, B=0x2,
    )
    assert child["kind"] == "or" and child["exclusive"] is True


def test_or_concrete_distinct_emits_exclusive_or():
    """An `or` block over distinct concrete heads emits the exclusive form."""
    child = _or_child(
        """
        opcode X = 0x9
        opcode A = 0x1
        opcode B = 0x2
        matcher m { produces MB
            op X @x -> or { op A @a ; op B @b }
            out { v: 1 }
        }
        """,
        X=0x9, A=0x1, B=0x2,
    )
    assert child["kind"] == "or" and child["exclusive"] is True


def test_or_with_predicate_emits_plain_or():
    reg = _registries_with_opcodes(X=0x9, A=0x1)
    reg.predicates["p"] = lambda b: True
    src = """
    opcode X = 0x9
    opcode A = 0x1
    predicate p
    matcher m { produces MB
        op X @x -> or { op A @a ; op A @a2 where p }
        out { v: 1 }
    }
    """
    art = emit_ir(compile_text(src, "test.mch", reg))
    m, nodes = art["matchers"][0], art["nodes"]
    child = nodes[nodes[m["root"]]["child"]]
    assert child["kind"] == "or" and child["exclusive"] is False


def test_or_nested_sequence_emits_plain_or():
    child = _or_child(
        """
        opcode X = 0x9
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        matcher m { produces MB
            op X @x -> or { op A @a ; (op B @b -> op C @c) }
            out { v: 1 }
        }
        """,
        X=0x9, A=0x1, B=0x2, C=0x3,
    )
    assert child["kind"] == "or" and child["exclusive"] is False


def test_xor_with_predicate_still_exclusive_or():
    reg = _registries_with_opcodes(X=0x9, A=0x1, B=0x2)
    reg.predicates["p"] = lambda b: True
    src = """
    opcode X = 0x9
    opcode A = 0x1
    opcode B = 0x2
    predicate p
    matcher m { produces MB
        op X @x -> xor { op A @a ; op B @a2 where p }
        out { v: 1 }
    }
    """
    art = emit_ir(compile_text(src, "test.mch", reg))
    m, nodes = art["matchers"][0], art["nodes"]
    child = nodes[nodes[m["root"]]["child"]]
    assert child["kind"] == "or" and child["exclusive"] is True


def test_nested_xor_outer_or_inner():
    """The outer `xor` is exclusive; the inner `or` has a predicate and stays plain."""
    reg = _registries_with_opcodes(X=0x9, A=0x1, B=0x2, C=0x3)
    reg.predicates["p"] = lambda b: True
    src = """
    opcode X = 0x9
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    predicate p
    matcher m { produces MB
        op X @x -> xor { op A @a ; or { op B @b ; op C @c where p } }
        out { v: 1 }
    }
    """
    art = emit_ir(compile_text(src, "test.mch", reg))
    m, nodes = art["matchers"][0], art["nodes"]
    outer = nodes[nodes[m["root"]]["child"]]
    assert outer["kind"] == "or" and outer["exclusive"] is True
    inner = nodes[outer["branches"][1]]
    assert inner["kind"] == "or" and inner["exclusive"] is False


def test_xor_redundant_heads_errors():
    with pytest.raises(CompileError) as exc:
        _or_child(
            """
            opcode X = 0x9
            opcode A = 0x1
            matcher m { produces MB
                op X @x -> xor { op A @a ; op A @a2 }
                out { v: 1 }
            }
            """,
            X=0x9, A=0x1,
        )
    assert any(
        d.code == "W202_EXCLUSIVE_OR_REDUNDANT_HEADS"
        for d in exc.value.bag.items
    )


@pytest.mark.parametrize("strategy, continuation", [("", "maybe $chain"), ("cyclic", "$chain")])
def test_recursive_rule_xor_errors(strategy, continuation):
    with pytest.raises(CompileError) as exc:
        _emit_one(f"""
            rule chain {strategy} = xor {{ op 2 -> {continuation} ; op 3 }}
            matcher m {{ produces MB
                op 1 @a -> $chain
                out {{ v: 1 }}
            }}
        """)
    assert any(
        d.code == "C017_RECURSIVE_RULE_XOR"
        and d.suggestion == "use `or`"
        for d in exc.value.bag.items
    )


# ==========================================================================
# T12 — mixed-direction chain lowering (spec §6.4, §6.5)
# ==========================================================================

def test_t12_shape1_anchor_headed_parent_edge():
    """`op A @a <- op B @b` — B is A's parent; nothing else."""
    m, nodes = _emit_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        matcher m { produces MB
            op A @a <- op B @b
            out { v: 1 }
        }
        """,
        A=0x1, B=0x2,
    )
    a = nodes[m["root"]]
    assert a["kind"] == "contract" and a["opcode"] == 0x1
    assert "child" not in a and "children" not in a
    b = nodes[a["parent"]]
    assert b["opcode"] == 0x2 and "parent" not in b


def test_t12_shape2_multihop_parent_chain():
    m, nodes = _emit_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        matcher m { produces MB
            op A @a <- op B @b <- op C @c
            out { v: 1 }
        }
        """,
        A=0x1, B=0x2, C=0x3,
    )
    a = nodes[m["root"]]
    assert "child" not in a and "children" not in a
    b = nodes[a["parent"]]
    assert b["opcode"] == 0x2
    c = nodes[b["parent"]]
    assert c["opcode"] == 0x3 and "parent" not in c


def test_t12_shape3_children_after_parent_edge_attach_to_parent():
    """`op A <- op B -> {C; D}` — the children set attaches to B (attach-to-left
    after a parent edge, spec §6.5), NOT to anchor A."""
    m, nodes = _emit_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        opcode D = 0x4
        matcher m { produces MB
            op A @a <- op B @b -> {op C @c; op D @d}
            out { v: 1 }
        }
        """,
        A=0x1, B=0x2, C=0x3, D=0x4,
    )
    a = nodes[m["root"]]
    assert "child" not in a and "children" not in a
    b = nodes[a["parent"]]
    assert b["opcode"] == 0x2
    assert "parent" not in b
    assert [nodes[i]["opcode"] for i in b["children"]] == [0x3, 0x4]


def test_t12_shape4_mid_chain_children_attach_to_left():
    """`op A -> op B -> {C; D}` — mid-chain children set attaches to B."""
    m, nodes = _emit_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        opcode D = 0x4
        matcher m { produces MB
            op A @a -> op B @b -> {op C @c; op D @d}
            out { v: 1 }
        }
        """,
        A=0x1, B=0x2, C=0x3, D=0x4,
    )
    a = nodes[m["root"]]
    assert "parent" not in a and "children" not in a
    b = nodes[a["child"]]
    assert b["opcode"] == 0x2
    assert [nodes[i]["opcode"] for i in b["children"]] == [0x3, 0x4]


@pytest.mark.parametrize(
    "declaration, pattern, following",
    [
        ("", "op 1 @a -> {op 2; op 3} -> op 4", "op 4"),
        ("", "op 1 @a -> op 2 -> {op 3} -> op 4", "op 4"),
        ("", "op 1 @a <- (op 2 -> {op 3} -> op 4)", "op 4"),
        ("", "op 1 @a -> or { op 2 -> {op 3} -> op 4; op 5 }", "op 4"),
        ("rule leg = op 2 -> {op 3} -> op 4", "op 1 @a -> $leg", "op 4"),
        ("", "op 1 @a -> {op 2} -> {op 3}", "{op 3}"),
        ("", "op 1 @a -> {op 2; maybe op 3} <- op 4", "op 4"),
        ("", "op 1 @a -> op 2 -> {op 3} <- op 4", "op 4"),
        ("", "op 1 @a <- (op 2 -> {op 3} <- op 4)", "op 4"),
        ("", "op 1 @a -> or { op 2 -> {op 3} <- op 4; op 5 }", "op 4"),
        ("rule leg = op 2 -> {op 3} <- op 4", "op 1 @a -> $leg", "op 4"),
    ],
    ids=[
        "matcher-head-child",
        "middle-of-child-chain-child",
        "parent-target-child",
        "alternative-body-child",
        "referenced-rule-child",
        "consecutive-groups-child",
        "matcher-head-parent",
        "middle-of-child-chain-parent",
        "parent-target-parent",
        "alternative-body-parent",
        "referenced-rule-parent",
    ],
)
def test_edge_after_children_block_rejected_with_c009(
    declaration, pattern, following,
):
    src = f"""
    {declaration}
    matcher m {{ produces MB
        {pattern}
        out {{ v: 1 }}
    }}
    """
    compiled = None
    with pytest.raises(CompileError) as exc:
        compiled = compile_text(src, "test.mch", _registries_with_opcodes())
    diagnostic, = exc.value.bag.items
    assert diagnostic.code == "C009_MIXED_CHILD_PARENT_CHAIN_UNSUPPORTED"
    assert src.startswith(following, diagnostic.span.start_off)
    assert compiled is None


def test_t12_shape6_child_then_parent_chain_rejected():
    """`op A -> op B <- op C` — rejected with C009 rather than dropping `<- op C`."""
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    matcher m { produces MB
        op A @a -> op B @b <- op C @c
        out { v: 1 }
    }
    """
    reg = _registries_with_opcodes(A=0x1, B=0x2, C=0x3)
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    codes = [d.code for d in exc.value.bag.items]
    assert "C009_MIXED_CHILD_PARENT_CHAIN_UNSUPPORTED" in codes, codes


def test_child_branch_off_parent_rejected():
    src = """
    matcher m { produces MB
        op A @a <- op P -> op S
        out { v: 1 }
    }
    """
    with pytest.raises(CompileError) as exc:
        _emit_one(src, A=1, P=2, S=3)
    diagnostic, = exc.value.bag.items
    assert diagnostic.code == "C009_MIXED_CHILD_PARENT_CHAIN_UNSUPPORTED"
    assert "op A @a <- (op P -> op S)" in diagnostic.message


def test_grouped_head_rejected():
    src = """
    matcher m { produces MB
        (op A @a -> op B) -> op C
        out { v: 1 }
    }
    """
    with pytest.raises(CompileError) as exc:
        _emit_one(src, A=1, B=2, C=3)
    diagnostic, = exc.value.bag.items
    assert diagnostic.code == "C009_MIXED_CHILD_PARENT_CHAIN_UNSUPPORTED"
    assert "op A @a -> op B -> op C" in diagnostic.message


def test_grouped_parent_child_branch_preserved():
    matcher, nodes = _emit_one(
        """
        matcher m { produces MB
            op A @a <- (op P -> op S)
            out { v: 1 }
        }
        """,
        A=1, P=2, S=3,
    )
    root = nodes[matcher["root"]]
    parent = nodes[root["parent"]]
    assert parent["opcode"] == 2
    assert nodes[parent["child"]]["opcode"] == 3


def test_rule_head_rejected():
    src = """
    rule head = op A
    matcher m { produces MB
        $head -> op B
        out { v: 1 }
    }
    """
    with pytest.raises(CompileError) as exc:
        _emit_one(src, A=1, B=2)
    diagnostic, = exc.value.bag.items
    assert diagnostic.code == "R007_ENTRY_VIOLATES_8_3"
    assert "head cannot be a rule reference" in diagnostic.message


# ==========================================================================
# T10 — opcode-set / mixed-kind group anchors (spec §8.3 M2)
# ==========================================================================

def test_t10_opcode_set_anchor_and_chain_edge():
    """`xor { op A @m ; op B @m } -> maybe op N @n`: opcode_set anchor over both heads,
    the chain edge off the group is a single optional child."""
    m, nodes = _emit_one(
        """
        opcode JettonMint = 0x1
        opcode MinterJettonMint = 0x2
        opcode JettonNotify = 0x3
        matcher jetton_mint { produces MB
            xor { op JettonMint @m ; op MinterJettonMint @m } -> maybe op JettonNotify @n
            out { v: 1 }
        }
        """,
        JettonMint=0x1, MinterJettonMint=0x2, JettonNotify=0x3,
    )
    assert m["name"] == "jetton_mint"
    assert m["anchor"] == {"kind": "opcode_set", "values": [0x1, 0x2]}
    root = nodes[m["root"]]
    assert root["kind"] == "any"
    child = nodes[root["child"]]
    assert child["kind"] == "contract" and child["opcode"] == 0x3
    assert child["optional"] is True
    assert "parent" not in root and "children" not in root


def test_t10_mixed_op_and_btype_set_anchor():
    """Branches mixing op and btype heads emit the `mixed` anchor form."""
    m, nodes = _emit_one(
        """
        opcode A = 0x1
        matcher m { produces MB
            xor { op A @x ; btype jetton_transfer @x }
            out { v: 1 }
        }
        """,
        A=0x1,
    )
    assert m["anchor"] == {
        "kind": "mixed",
        "branches": [{"op": 0x1}, {"btype": "jetton_transfer"}],
    }
    assert nodes[m["root"]]["kind"] == "any"


# ==========================================================================
# Legal relation-block replacements for edges after a children block
# ==========================================================================

@pytest.mark.parametrize(("edge", "field"), [("->", "child"), ("<-", "parent")])
def test_relation_block_is_legal_replacement_for_edge_after_children_block(
    edge, field,
):
    matcher, nodes = _emit_one(
        "matcher m { produces MB\n"
        f"op 1 @a {{ -> {{op 2 @b; maybe op 3 @c}}; {edge} op 4 @related }}\n"
        "out { v: 1 } }"
    )
    owner = nodes[matcher["root"]]
    related = nodes[owner[field]]
    assert related["opcode"] == 4

    child_nodes = [nodes[i] for i in owner["children"]]
    assert [n["opcode"] for n in child_nodes] == [2, 3]
    assert [n.get("optional", False) for n in child_nodes] == [False, True]


# ==========================================================================
# T33 — inline `where (expr)` must be sync-evaluable (C013)
# ==========================================================================

def _c013_codes(src: str) -> list[str]:
    reg = _registries_with_opcodes(A=0x1, B=0x2)
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    return [d.code for d in exc.value.bag.items]


def test_where_expr_lookup_is_a_compile_error():
    src = """
    matcher m { produces MB
        op A @a -> op B @b where (lookup jetton_wallet(.owner) == null)
        out { v: 1 }
    }
    """
    assert "C013_WHERE_EXPR_NOT_SYNC" in _c013_codes(src)


def test_where_expr_host_fn_call_is_a_compile_error():
    src = """
    matcher m { produces MB
        op A @a -> op B @b where (my_host_fn(.owner) == 1)
        out { v: 1 }
    }
    """
    assert "C013_WHERE_EXPR_NOT_SYNC" in _c013_codes(src)


def test_where_expr_may_reference_anchor_capture():
    src = """
    matcher m { produces MB
        op A @a -> op B @b where (.owner == a.data.owner)
        out { v: 1 }
    }
    """
    reg = _registries_with_opcodes(A=0x1, B=0x2)
    compile_text(src, "test.mch", reg)


def test_where_expr_non_anchor_capture_is_a_compile_error():
    src = """
    matcher m { produces MB
        op A @a -> op B @b where (.owner == b.data.owner)
        out { v: 1 }
    }
    """
    assert "C015_WHERE_EXPR_CAPTURE_NOT_ENTRY" in _c013_codes(src)


def test_where_expr_on_group_anchor_branch_is_a_compile_error():
    """Group-anchor branches carry only named `where` predicates in the anchor
    record; an inline expr there would be silently dropped — reject."""
    src = """
    matcher m { produces MB
        xor { op A @a where (.x == 1) ; op B @a }
        out { v: 1 }
    }
    """
    assert "C013_WHERE_EXPR_NOT_SYNC" in _c013_codes(src)


# ==========================================================================
# `peek` subtree closure
# ==========================================================================

def _compile_error_codes(src: str, **opcodes: int) -> list[str]:
    reg = _registries_with_opcodes(**opcodes)
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    return [d.code for d in exc.value.bag.items]


@pytest.mark.parametrize(
    "pattern",
    [
        "op A @a -> peek op B @b -> op C @c",
        "op A @a -> peek op B @b -> { op C @c }",
        "op A @a <- peek op B @b <- op C @c",
    ],
)
def test_peek_rejects_consuming_descendants_on_structural_edges(pattern: str):
    src = f"""
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    matcher m {{ produces MB
        {pattern}
        out {{ v: 1 }}
    }}
    """
    assert "C014_PEEK_SUBTREE_CONSUMES" in _compile_error_codes(
        src, A=0x1, B=0x2, C=0x3,
    )


def test_peek_rejects_consuming_descendant_reached_through_inlined_rule_branches():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    rule branches = xor { peek op B ; op C }
    matcher m { produces MB
        op A @a -> peek $branches
        out { v: 1 }
    }
    """
    assert "C014_PEEK_SUBTREE_CONSUMES" in _compile_error_codes(
        src, A=0x1, B=0x2, C=0x3,
    )


def test_peek_rejects_consuming_frontier_recursion_step_and_exit():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    rule chain = or { op B -> maybe $chain ; op C }
    matcher m { produces MB
        op A @a -> peek $chain
        out { v: 1 }
    }
    """
    assert "C014_PEEK_SUBTREE_CONSUMES" in _compile_error_codes(
        src, A=0x1, B=0x2, C=0x3,
    )


def test_all_peek_descendants_emit_successfully():
    m, nodes = _emit_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        matcher m { produces MB
            op A @a -> peek op B @b -> peek op C @c
            out { v: 1 }
        }
        """,
        A=0x1, B=0x2, C=0x3,
    )
    root = nodes[m["root"]]
    assert "peek" not in root
    child = nodes[root["child"]]
    grandchild = nodes[child["child"]]
    assert child["peek"] is True
    assert grandchild["peek"] is True


def test_peek_on_cyclic_self_reference_reuses_c012():
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    rule loop cyclic = or { op B -> peek $loop ; op C }
    matcher m { produces MB
        op A @a -> $loop
        out { v: 1 }
    }
    """
    assert "C012_CYCLIC_RULE_SHAPE" in _compile_error_codes(
        src, A=0x1, B=0x2, C=0x3,
    )


@pytest.mark.parametrize("head", ["op 1", "btype thing", "pred p", "op [1, 2]"])
def test_head_relation_block_lowers_all_three_fields(head):
    reg = _registries_with_opcodes()
    reg.predicates["p"] = lambda b: True
    src = (
        "matcher m { produces MB\n"
        + head + " @a { -> op 3; -> { op 4 }; <- op 5 }\nout { v: 1 } }"
    )
    artifact = emit_ir(compile_text(src, "test.mch", reg))
    nodes = artifact["nodes"]
    root = nodes[artifact["matchers"][0]["root"]]
    assert nodes[root["child"]]["opcode"] == 3
    assert [nodes[i]["opcode"] for i in root["children"]] == [4]
    assert nodes[root["parent"]]["opcode"] == 5


def test_relation_block_on_child_allows_parent_without_c009():
    matcher, nodes = _emit_one(
        "matcher m { produces MB\n"
        "op 1 @r -> op 2 { -> op 3; <- op 4 } \nout { v: 1 } }"
    )
    child = nodes[nodes[matcher["root"]]["child"]]
    assert child["opcode"] == 2
    assert nodes[child["child"]]["opcode"] == 3
    assert nodes[child["parent"]]["opcode"] == 4


def test_relation_block_keeps_peek_subtree_validation():
    src = (
        "matcher m { produces MB\n"
        "op 1 @a -> peek op 2 { -> { op 3 } } \nout { v: 1 } }"
    )
    assert "C014_PEEK_SUBTREE_CONSUMES" in _compile_error_codes(src)


def test_relation_block_keeps_cyclic_capture_validation():
    src = (
        "rule leg cyclic = op 2 { -> or { op 3 @hidden -> $leg ; op 4 } }\n"
        "matcher m { produces MB\n op 1 @a -> $leg \nout { v: 1 } }"
    )
    assert "C012_CYCLIC_RULE_SHAPE" in _compile_error_codes(src)
