"""Structural lowering conformance, asserted on the emitted IR JSON.

T29 stage 1 retired the compiled runtime, so these tests no longer inspect
BlockMatcher wiring (child_matcher/parent_matcher/children_matchers/test_self).
Instead each spec is compiled and emitted (emit_ir) and the resulting node graph
+ anchor record is asserted directly — the same lowering contract, expressed on
the artifact the frontend produces. Compile-time rejections (C005/C009/C010/C013)
stay as-is: they never reach a runtime. Anchor membership/where behavior (R09/R10
mixed + pred anchors) is asserted in tests/mch_ir/test_anchor_r09.py and
test_anchor_r10.py; runtime binding of these shapes lives in tests/mch_ir/.
"""
from __future__ import annotations

import pytest

from indexer.events.mch.compile import compile_text
from indexer.events.mch.diagnostics import CompileError, Severity
from indexer.events.mch.ir_emit import emit_ir
from indexer.events.mch.registry import Registries


def _registries_with_opcodes(**opcodes: int) -> Registries:
    r = Registries()
    r.opcodes.update(opcodes)
    return r


def _with_builder(reg: Registries, name: str) -> Registries:
    @reg.register_builder(name)
    async def _build(m):  # noqa: ARG001
        return None
    return reg


def _with_block_type(reg: Registries, name: str) -> Registries:
    reg.block_types[name] = type(name, (), {})
    return reg


def _emit_one(src: str, **opcodes: int):
    """Compile one matcher and return (matcher_record, nodes) from its IR."""
    reg = _registries_with_opcodes(**opcodes)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    art = emit_ir(compile_text(src, "test.mch", reg), ["test.mch"])
    return art["matchers"][0], art["nodes"]


# ==========================================================================
# Atoms and single-edge chains
# ==========================================================================

def test_single_op_node_emits_contract_root_and_opcode_set_anchor():
    src = """
    opcode JettonTransfer = 0x0f8a7ea5

    matcher jetton_transfer {
        produces JettonTransferBlock
        build    build_jetton_transfer
        entry    @t

        op JettonTransfer @t
    }
    """
    reg = _registries_with_opcodes(JettonTransfer=0x0F8A7EA5)
    _with_block_type(reg, "JettonTransferBlock")
    _with_builder(reg, "build_jetton_transfer")
    art = emit_ir(compile_text(src, "test.mch", reg), ["test.mch"])
    m, nodes = art["matchers"][0], art["nodes"]

    assert m["name"] == "jetton_transfer"
    assert m["anchor"] == {"kind": "opcode_set", "values": [0x0F8A7EA5]}
    root = nodes[m["root"]]
    assert root["kind"] == "contract"
    assert root["opcode"] == 0x0F8A7EA5
    assert root["capture"] == "t"


def test_linear_chain_emits_child_chain():
    m, nodes = _emit_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        matcher m {
            produces MB
            build    build_m
            entry    @a
            op A @a -> op B @b -> op C @c
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
            build    build_m
            entry    @a
            op A @a -> maybe op B @b
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
            build    build_m
            entry    @a
            op A @a -> ( op B @b | op C @c )
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
            build    build_m
            entry    @a
            op A @a -> btype jetton_transfer @j
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
            build    build_m
            entry    @j
            btype jetton_transfer @j
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
            build    build_m
            entry    @a
            op A @a -> {
                op B @b
                op C @c
            }
        }
        """,
        A=0x1, B=0x2, C=0x3,
    )
    root = nodes[m["root"]]
    assert [nodes[i]["opcode"] for i in root["children"]] == [0x2, 0x3]
    assert "child" not in root


def test_recursive_rule_emits_recursive_node():
    m, nodes = _emit_one(
        """
        opcode A      = 0x1
        opcode B      = 0x2
        opcode Anchor = 0x3

        rule swap_step =
            op A @step -> ( maybe $swap_step | op B @term )

        matcher m {
            produces MB
            build    build_m
            entry    @anchor

            op Anchor @anchor -> $swap_step
        }
        """,
        A=0x1, B=0x2, Anchor=0x3,
    )
    assert any(n["kind"] == "recursive" for n in nodes), "expected a recursive node in the graph"


def test_multi_hop_parent_chain_emits_parent_chain():
    """`op A @a <- op B <- op C` lowers to a chain of parent edges; leaf is C."""
    m, nodes = _emit_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        matcher m {
            produces MB
            build    build_m
            entry    @a
            op A @a <- op B @b <- op C @c
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


def test_indirect_recursion_rejected():
    """Mutual recursion across two rules is not supported in v0; compiler errors."""
    src = """
    opcode A      = 0x1
    opcode B      = 0x2
    opcode Anchor = 0x3

    rule r_a = op A @a -> maybe $r_b
    rule r_b = op B @b -> maybe $r_a

    matcher m {
        produces MB
        build    build_m
        entry    @anchor

        op Anchor @anchor -> $r_a
    }
    """
    reg = _registries_with_opcodes(A=0x1, B=0x2, Anchor=0x3)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    codes = [d.code for d in exc.value.bag.items if d.severity is Severity.ERROR]
    assert "C005_INDIRECT_RECURSION_UNSUPPORTED" in codes, codes


# ==========================================================================
# XOR separator: exclusive flag on the emitted `or` node
# ==========================================================================

def _or_child(src: str, **opcodes: int) -> dict:
    m, nodes = _emit_one(src, **opcodes)
    return nodes[nodes[m["root"]]["child"]]


def test_caret_emits_exclusive_or():
    child = _or_child(
        """
        opcode X = 0x9
        opcode A = 0x1
        opcode B = 0x2
        matcher m { produces MB build build_m entry @x
            op X @x -> (op A @a ^ op B @b) }
        """,
        X=0x9, A=0x1, B=0x2,
    )
    assert child["kind"] == "or" and child["exclusive"] is True


def test_pipe_concrete_distinct_emits_exclusive_or():
    """Heuristic: `|` over distinct concrete heads fires the exclusive form."""
    child = _or_child(
        """
        opcode X = 0x9
        opcode A = 0x1
        opcode B = 0x2
        matcher m { produces MB build build_m entry @x
            op X @x -> (op A @a | op B @b) }
        """,
        X=0x9, A=0x1, B=0x2,
    )
    assert child["kind"] == "or" and child["exclusive"] is True


def test_pipe_with_predicate_emits_plain_or():
    reg = _registries_with_opcodes(X=0x9, A=0x1)
    reg.predicates["p"] = lambda b: True
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    src = """
    opcode X = 0x9
    opcode A = 0x1
    predicate p
    matcher m { produces MB build build_m entry @x
        op X @x -> (op A @a | op A @a2 where p) }
    """
    art = emit_ir(compile_text(src, "test.mch", reg), ["test.mch"])
    m, nodes = art["matchers"][0], art["nodes"]
    child = nodes[nodes[m["root"]]["child"]]
    assert child["kind"] == "or" and child["exclusive"] is False


def test_pipe_nested_sequence_emits_plain_or():
    child = _or_child(
        """
        opcode X = 0x9
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        matcher m { produces MB build build_m entry @x
            op X @x -> (op A @a | (op B @b -> op C @c)) }
        """,
        X=0x9, A=0x1, B=0x2, C=0x3,
    )
    assert child["kind"] == "or" and child["exclusive"] is False


def test_caret_with_predicate_still_exclusive_or():
    reg = _registries_with_opcodes(X=0x9, A=0x1)
    reg.predicates["p"] = lambda b: True
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    src = """
    opcode X = 0x9
    opcode A = 0x1
    predicate p
    matcher m { produces MB build build_m entry @x
        op X @x -> (op A @a ^ op A @a2 where p) }
    """
    art = emit_ir(compile_text(src, "test.mch", reg), ["test.mch"])
    m, nodes = art["matchers"][0], art["nodes"]
    child = nodes[nodes[m["root"]]["child"]]
    assert child["kind"] == "or" and child["exclusive"] is True


def test_nested_caret_outer_pipe_inner():
    """Outer `^` is exclusive; the inner `|` branch (predicate-blocked) is plain."""
    reg = _registries_with_opcodes(X=0x9, A=0x1, B=0x2, C=0x3)
    reg.predicates["p"] = lambda b: True
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    src = """
    opcode X = 0x9
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    predicate p
    matcher m { produces MB build build_m entry @x
        op X @x -> (op A @a ^ (op B @b | op C @c where p)) }
    """
    art = emit_ir(compile_text(src, "test.mch", reg), ["test.mch"])
    m, nodes = art["matchers"][0], art["nodes"]
    outer = nodes[nodes[m["root"]]["child"]]
    assert outer["kind"] == "or" and outer["exclusive"] is True
    inner = nodes[outer["branches"][1]]
    assert inner["kind"] == "or" and inner["exclusive"] is False


def test_caret_redundant_heads_warns(monkeypatch):
    """`(op A ^ op A)`: W202 emitted; the node still lowers to exclusive `or`."""
    from indexer.events.mch import diagnostics as diag_module
    warnings_captured: list = []

    original_warning = diag_module.DiagnosticBag.warning

    def _patched_warning(self, code, message, span, suggestion=None):
        warnings_captured.append(code)
        return original_warning(self, code, message, span, suggestion)

    monkeypatch.setattr(diag_module.DiagnosticBag, "warning", _patched_warning)

    child = _or_child(
        """
        opcode X = 0x9
        opcode A = 0x1
        matcher m { produces MB build build_m entry @x
            op X @x -> (op A @a ^ op A @a2) }
        """,
        X=0x9, A=0x1,
    )
    assert "W202_EXCLUSIVE_OR_REDUNDANT_HEADS" in warnings_captured, warnings_captured
    assert child["kind"] == "or" and child["exclusive"] is True


# ==========================================================================
# T12 — mixed-direction chain lowering (spec §6.4, §6.5)
# ==========================================================================

def test_t12_shape1_anchor_headed_parent_edge():
    """`op A @a <- op B @b` — B is A's parent; nothing else."""
    m, nodes = _emit_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        matcher m { produces MB build build_m entry @a
            op A @a <- op B @b }
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
        matcher m { produces MB build build_m entry @a
            op A @a <- op B @b <- op C @c }
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
        matcher m { produces MB build build_m entry @a
            op A @a <- op B @b -> {op C @c; op D @d} }
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
        matcher m { produces MB build build_m entry @a
            op A @a -> op B @b -> {op C @c; op D @d} }
        """,
        A=0x1, B=0x2, C=0x3, D=0x4,
    )
    a = nodes[m["root"]]
    assert "parent" not in a and "children" not in a
    b = nodes[a["child"]]
    assert b["opcode"] == 0x2
    assert [nodes[i]["opcode"] for i in b["children"]] == [0x3, 0x4]


def test_t12_shape5_children_block_does_not_advance_cursor():
    """`op A -> {B; C} -> op D` — A has BOTH children=[B,C] AND child=D."""
    m, nodes = _emit_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        opcode D = 0x4
        matcher m { produces MB build build_m entry @a
            op A @a -> {op B @b; op C @c} -> op D @d }
        """,
        A=0x1, B=0x2, C=0x3, D=0x4,
    )
    a = nodes[m["root"]]
    assert "parent" not in a
    assert [nodes[i]["opcode"] for i in a["children"]] == [0x2, 0x3]
    d = nodes[a["child"]]
    assert d["opcode"] == 0x4 and "child" not in d


def test_t12_shape6_child_then_parent_chain_rejected():
    """`op A -> op B <- op C` — rejected with C009 rather than dropping `<- op C`."""
    src = """
    opcode A = 0x1
    opcode B = 0x2
    opcode C = 0x3
    matcher m { produces MB build build_m entry @a
        op A @a -> op B @b <- op C @c }
    """
    reg = _registries_with_opcodes(A=0x1, B=0x2, C=0x3)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    codes = [d.code for d in exc.value.bag.items if d.severity is Severity.ERROR]
    assert "C009_MIXED_CHILD_PARENT_CHAIN_UNSUPPORTED" in codes, codes


# ==========================================================================
# T10 — opcode-set / mixed-kind group anchors (spec §8.3 M2)
# ==========================================================================

def test_t10_opcode_set_anchor_and_chain_edge():
    """`(op A @m ^ op B @m) -> maybe op N @n`: opcode_set anchor over both heads,
    the chain edge off the group is a single optional child."""
    m, nodes = _emit_one(
        """
        opcode JettonMint = 0x1
        opcode MinterJettonMint = 0x2
        opcode JettonNotify = 0x3
        matcher jetton_mint { produces MB build build_m entry @m
            (op JettonMint @m ^ op MinterJettonMint @m) -> maybe op JettonNotify @n }
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
        matcher m { produces MB build build_m entry @x
            (op A @x ^ btype jetton_transfer @x) }
        """,
        A=0x1,
    )
    assert m["anchor"] == {
        "kind": "mixed",
        "branches": [{"op": 0x1}, {"btype": "jetton_transfer"}],
    }
    assert nodes[m["root"]]["kind"] == "any"


# ==========================================================================
# T15 — children set then a parent edge (dedust-swap shape, spec §6.5)
# ==========================================================================

def test_t15_shape7_children_block_then_parent_edge():
    """`op A -> {B; maybe C} <- op P` — the children set does not advance the
    cursor, so the parent edge still belongs to A: A gets BOTH children=[B, C]
    AND parent=P."""
    m, nodes = _emit_one(
        """
        opcode A = 0x1
        opcode B = 0x2
        opcode C = 0x3
        opcode P = 0x4
        matcher m { produces MB build build_m entry @a
            op A @a -> {op B @b; maybe op C @c} <- op P @p }
        """,
        A=0x1, B=0x2, C=0x3, P=0x4,
    )
    a = nodes[m["root"]]
    assert "child" not in a
    child_nodes = [nodes[i] for i in a["children"]]
    assert [n["opcode"] for n in child_nodes] == [0x2, 0x3]
    assert [n.get("optional", False) for n in child_nodes] == [False, True]
    p = nodes[a["parent"]]
    assert p["opcode"] == 0x4
    assert "children" not in p


# ==========================================================================
# T04 — trailing parent edge after a children block is preserved
# ==========================================================================

def test_t04_flat_parent_chain_children_then_parent_edge():
    """`@a <- op X -> {op D} <- op Y` (flat) lowers identically to the grouped
    form `<- ( op X -> {op D} <- op Y )` — X gets BOTH children=[D] AND parent=Y."""
    flat, flat_nodes = _emit_one(
        """
        opcode A = 0x1
        opcode X = 0x2
        opcode D = 0x3
        opcode Y = 0x4
        matcher m { produces MB build build_m entry @a
            op A @a <- op X @x -> {op D @d} <- op Y @y }
        """,
        A=0x1, X=0x2, D=0x3, Y=0x4,
    )
    grouped, grouped_nodes = _emit_one(
        """
        opcode A = 0x1
        opcode X = 0x2
        opcode D = 0x3
        opcode Y = 0x4
        matcher m { produces MB build build_m entry @a
            op A @a <- ( op X @x -> {op D @d} <- op Y @y ) }
        """,
        A=0x1, X=0x2, D=0x3, Y=0x4,
    )
    assert flat_nodes == grouped_nodes
    a = flat_nodes[flat["root"]]
    assert "child" not in a and "children" not in a
    x = flat_nodes[a["parent"]]
    assert x["opcode"] == 0x2
    assert [flat_nodes[i]["opcode"] for i in x["children"]] == [0x3]
    y = flat_nodes[x["parent"]]
    assert y["opcode"] == 0x4 and "parent" not in y and "children" not in y


def test_t04_flat_multi_hop_children_then_parent_edge():
    """`@a -> {op K} <- op T <- op X -> {op D} <- op Y` — the second hop's
    `<- Y` after its children block survives the multi-hop parent chain."""
    m, nodes = _emit_one(
        """
        opcode A = 0x1
        opcode K = 0x5
        opcode T = 0x6
        opcode X = 0x2
        opcode D = 0x3
        opcode Y = 0x4
        matcher m { produces MB build build_m entry @a
            op A @a -> {op K @k} <- op T @t <- op X @x -> {op D @d} <- op Y @y }
        """,
        A=0x1, K=0x5, T=0x6, X=0x2, D=0x3, Y=0x4,
    )
    a = nodes[m["root"]]
    assert [nodes[i]["opcode"] for i in a["children"]] == [0x5]
    t = nodes[a["parent"]]
    assert t["opcode"] == 0x6
    x = nodes[t["parent"]]
    assert x["opcode"] == 0x2
    assert [nodes[i]["opcode"] for i in x["children"]] == [0x3]
    y = nodes[x["parent"]]
    assert y["opcode"] == 0x4 and "parent" not in y


def test_t04_upstream_atoms_before_anchor_rejected():
    """`op Up @up -> op Anchor @anchor` with entry @anchor — the upstream `op Up`
    would be silently dropped; the compiler rejects it with C010."""
    src = """
    opcode Up     = 0x1
    opcode Anchor = 0x2
    matcher m { produces MB build build_m entry @anchor
        op Up @up -> op Anchor @anchor }
    """
    reg = _registries_with_opcodes(Up=0x1, Anchor=0x2)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    errors = [d for d in exc.value.bag.items if d.severity is Severity.ERROR]
    codes = [d.code for d in errors]
    assert "C010_UPSTREAM_ATOMS_UNSUPPORTED" in codes, codes
    diag = next(d for d in errors if d.code == "C010_UPSTREAM_ATOMS_UNSUPPORTED")
    assert "Up" in src[diag.span.start_off:diag.span.end_off]


# ==========================================================================
# T33 — inline `where (expr)` must be sync-evaluable (C013)
# ==========================================================================

def _c013_codes(src: str) -> list[str]:
    reg = _registries_with_opcodes(A=0x1, B=0x2)
    _with_block_type(reg, "MB")
    _with_builder(reg, "build_m")
    with pytest.raises(CompileError) as exc:
        compile_text(src, "test.mch", reg)
    return [d.code for d in exc.value.bag.items if d.severity is Severity.ERROR]


def test_where_expr_lookup_is_a_compile_error():
    src = """
    matcher m { produces MB build build_m entry @a
        op A @a -> op B @b where (lookup jetton_wallet(.owner) == null) }
    """
    assert "C013_WHERE_EXPR_NOT_SYNC" in _c013_codes(src)


def test_where_expr_host_fn_call_is_a_compile_error():
    src = """
    matcher m { produces MB build build_m entry @a
        op A @a -> op B @b where (my_host_fn(.owner) == 1) }
    """
    assert "C013_WHERE_EXPR_NOT_SYNC" in _c013_codes(src)


def test_where_expr_on_group_anchor_branch_is_a_compile_error():
    """Group-anchor branches carry only named `where` predicates in the anchor
    record; an inline expr there would be silently dropped — reject."""
    src = """
    matcher m { produces MB build build_m entry @a
        (op A @a where (.x == 1) ^ op B @a) }
    """
    assert "C013_WHERE_EXPR_NOT_SYNC" in _c013_codes(src)
