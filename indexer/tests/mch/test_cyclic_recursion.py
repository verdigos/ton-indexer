"""Cyclic recursion compiler and IR emission conformance."""
from __future__ import annotations

import pytest

from indexer.events.mch.compile import compile_text
from indexer.events.mch.diagnostics import CompileError
from indexer.events.mch.ir_emit import emit_ir
from indexer.events.mch.registry import Registries


def _registries() -> Registries:
    reg = Registries()

    @reg.register_fn("record_match")
    async def record_match(_fields):
        return True

    return reg


# S = swap-like head, P = payout-like, T = hop transfer that continues to the
# next S, and F = final transfer.
_CYCLIC_SRC = """
opcode S = 0x1
opcode P = 0x2
opcode T = 0x3
opcode F = 0x4

rule hop cyclic = (
    op S -> op P -> or { op T -> $hop
                         op F }
)

matcher cyc {
    produces CB

    op S @s -> op P -> or { op T -> $hop
                            op F }
    reject when record_match({s: s, _mch_consumed: consumed})
    out { v: 1 }
}
"""


def _compile_cyclic():
    reg = _registries()
    compiled = compile_text(_CYCLIC_SRC, "cyc.mch", reg)
    return compiled


def test_cyclic_rule_compiles_without_r006():
    compiled = _compile_cyclic()
    assert [c.name for c in compiled] == ["cyc"]


def test_cyclic_ir_emits_strategy_and_back_edge():
    compiled = _compile_cyclic()
    art = emit_ir(compiled)
    rec = [n for n in art["nodes"] if n.get("kind") == "recursive"]
    assert rec and all(n.get("strategy") == "cyclic" for n in rec)
    assert all("exit" not in n for n in rec)
    # The body root must be reachable from itself (back-edge cycle).
    step = rec[0]["step"]
    seen, frontier = set(), [step]
    cycle = False
    while frontier:
        i = frontier.pop()
        node = art["nodes"][i]
        for key in ("child", "parent", "step", "exit"):
            if key in node:
                ref = node[key]
                if ref == step:
                    cycle = True
                if ref not in seen:
                    seen.add(ref)
                    frontier.append(ref)
        for ref in node.get("children", []) + node.get("branches", []):
            if ref == step:
                cycle = True
            if ref not in seen:
                seen.add(ref)
                frontier.append(ref)
    assert cycle, "no back-edge to the body root in the emitted IR"


def _expect_c012(src: str) -> None:
    reg = _registries()
    with pytest.raises(CompileError) as ei:
        compile_text(src, "bad.mch", reg)
    codes = [d.code for d in ei.value.bag.items]
    assert "C012_CYCLIC_RULE_SHAPE" in codes, codes


_BAD_TEMPLATE = """
opcode S = 0x1
opcode P = 0x2
opcode T = 0x3
opcode F = 0x4

rule hop cyclic = (
    {rule_body}
)

matcher bad {{
    produces CB

    op S @s -> op P -> $hop
    reject when record_match({{s: s, _mch_consumed: consumed}})
    out {{ v: 1 }}
}}
"""


def test_c012_capture_inside_cyclic_body():
    _expect_c012(_BAD_TEMPLATE.format(
        rule_body="op S -> op P @cap -> or { op T -> $hop ; op F }"))


def test_c012_body_is_only_the_self_reference():
    _expect_c012(_BAD_TEMPLATE.format(rule_body="$hop"))


def test_c012_self_ref_not_chain_tail():
    _expect_c012(_BAD_TEMPLATE.format(
        rule_body="op S -> op P -> or { op T -> $hop -> op F ; op F }"))


def test_c012_maybe_self_ref_in_body():
    _expect_c012(_BAD_TEMPLATE.format(
        rule_body="op S -> op P -> or { op T -> maybe $hop ; op F }"))


def test_c012_other_recursive_rule_in_body():
    src = """
    opcode S = 0x1
    opcode P = 0x2
    opcode T = 0x3
    opcode F = 0x4

    rule other = or { op T -> maybe $other ; op F }

    rule hop cyclic = (
        op S -> op P -> or { op T -> $hop
                             op T -> $other
                             op F }
    )

    matcher bad {
        produces CB

        op S @s -> op P -> $hop
        reject when record_match({s: s, _mch_consumed: consumed})
        out { v: 1 }
    }
    """
    _expect_c012(src)
