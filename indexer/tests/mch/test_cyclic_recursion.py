"""Cyclic recursion strategy conformance ([M2R, R11/T28], spec §10.3).

Covers:
- `rule NAME cyclic = …` parses, compiles without R006 (termination is a
  non-recursive alternative branch, not `maybe $self`), and matches through
  BOTH engines (compiled path and v2 IR) identically — including the
  mandatory-continuation shape the frontier strategy cannot express
  (step and final branches sharing a prefix; T28 premature-exit).
- C012 shape diagnostics: captures inside the body, self-ref not at chain
  tail, `maybe`-wrapped in-body self-ref, reference to another recursive rule.
- IR loader validation: unknown strategy, cyclic + exit, captures reachable
  inside a cyclic body.
- The T28 "maybe inside an alternative" quirk, pinned: branch-level
  optionality is IGNORED by alternative matching — a losing `maybe` branch
  does not let the alternative succeed empty (legacy OrMatcher.try_build
  ignores `optional`; both mch engines reproduce that).
"""
from __future__ import annotations

import pytest

from indexer.events import mch_ir
from indexer.events.blocks.basic_matchers import ContractMatcher, OrMatcher
from indexer.events.mch.compile import compile_text
from indexer.events.mch.diagnostics import CompileError
from indexer.events.mch.ir_emit import emit_ir
from indexer.events.mch.registry import Registries
from indexer.events.mch_ir.loader import IrLoadError

from tests.mch.conftest import BlockSpec


# ---------------------------------------------------------------------------
# Scaffolding
# ---------------------------------------------------------------------------
def _registries(opcodes: dict[str, int], builder_name: str, produces: str) -> tuple[Registries, list]:
    captured: list = []
    reg = Registries()
    reg.opcodes.update(opcodes)
    reg.block_types[produces] = type(produces, (), {})

    @reg.register_builder(builder_name)
    async def _build(m):
        captured.append(m)
        return None
    return reg, captured


# S = swap-like head, P = payout-like, T = hop transfer (MUST continue to the
# next S), F = final transfer. Step branch (T -> $r) and final branch (F) hang
# off the same P — with T deliberately sharing nothing here; the tonco-shaped
# prefix-sharing case is exercised below via the same opcode for T and F.
_CYCLIC_SRC = """
opcode S = 0x1
opcode P = 0x2
opcode T = 0x3
opcode F = 0x4

rule hop cyclic = (
    op S -> op P -> ( op T -> $hop
                    | op F )
)

matcher cyc {
    produces CB
    build    build_cyc
    entry    @s

    op S @s -> op P -> ( op T -> $hop
                       | op F )
}
"""


def _compile_cyclic():
    reg, captured = _registries({}, "build_cyc", "CB")
    compiled = compile_text(_CYCLIC_SRC, "cyc.mch", reg)
    return compiled, reg, captured


def _two_hop_tree(build_block_tree):
    # S -> P -> T -> S -> P -> F : one full descent through the back-edge.
    return build_block_tree(BlockSpec(opcode=0x1, children=[
        BlockSpec(opcode=0x2, children=[
            BlockSpec(opcode=0x3, children=[
                BlockSpec(opcode=0x1, children=[
                    BlockSpec(opcode=0x2, children=[
                        BlockSpec(opcode=0x4),
                    ]),
                ]),
            ]),
        ]),
    ]))


def _count_blocks(root) -> int:
    n, stack = 0, [root]
    while stack:
        b = stack.pop()
        n += 1
        stack.extend(b.next_blocks)
    return n


# ---------------------------------------------------------------------------
# Compile + match: both engines
# ---------------------------------------------------------------------------
def test_cyclic_rule_compiles_without_r006():
    compiled, _reg, _c = _compile_cyclic()
    assert [c.name for c in compiled] == ["cyc"]


def test_cyclic_ir_emits_strategy_and_back_edge():
    compiled, _reg, _c = _compile_cyclic()
    art = emit_ir(compiled, ["cyc.mch"])
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


@pytest.mark.asyncio
async def test_cyclic_descent_matches_multi_hop_both_engines(build_block_tree):
    compiled, reg, captured = _compile_cyclic()
    tree = _two_hop_tree(build_block_tree)

    m = compiled[0].matcher
    result = await m.try_build(tree)
    assert result is not None and len(captured) == 1
    # Whole chain consumed: anchor S,P,T + inner S,P,F.
    assert len(captured[0]._mch_consumed) == _count_blocks(tree) == 6

    art = mch_ir.load_artifact(emit_ir(compiled, ["cyc.mch"]))
    reg2, captured2 = _registries({}, "build_cyc", "CB")
    ir_m = {x.name: x for x in mch_ir.build_matchers(art, reg2)}["cyc"]
    ir_result = await ir_m.try_build(_two_hop_tree(build_block_tree))
    assert ir_result is not None and len(captured2) == 1
    assert len(captured2[0]._mch_consumed) == 6


@pytest.mark.asyncio
async def test_cyclic_mandatory_continuation_disambiguates_shared_prefix(build_block_tree):
    """The T28 shape: hop and final branches share their head (same opcode).
    The hop branch must win only when the chain actually continues to the next
    S — looking down the chain, which the frontier walk cannot do."""
    src = """
    opcode S = 0x1
    opcode P = 0x2
    opcode T = 0x3

    rule hop cyclic = (
        op S -> op P -> ( op T -> $hop
                        | op T )
    )

    matcher pfx {
        produces CB
        build    build_pfx
        entry    @s

        op S @s -> op P -> ( op T -> $hop
                           | op T )
    }
    """
    def hop_chain(depth: int):
        spec = BlockSpec(opcode=0x3)  # terminal T, no continuation
        for _ in range(depth):
            spec = BlockSpec(opcode=0x3, children=[
                BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x2, children=[spec])]),
            ])
        return BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x2, children=[spec])])

    for factory in ("compiled", "ir"):
        reg, captured = _registries({}, "build_pfx", "CB")
        compiled = compile_text(src, "pfx.mch", reg)
        if factory == "compiled":
            m = compiled[0].matcher
            use_captured = captured
        else:
            art = mch_ir.load_artifact(emit_ir(compiled, ["pfx.mch"]))
            reg2, use_captured = _registries({}, "build_pfx", "CB")
            m = {x.name: x for x in mch_ir.build_matchers(art, reg2)}["pfx"]
        tree = build_block_tree(hop_chain(2))  # 3 levels: S P T / S P T / S P T(final)
        result = await m.try_build(tree)
        assert result is not None and len(use_captured) == 1, factory
        assert len(use_captured[0]._mch_consumed) == 9, factory


# ---------------------------------------------------------------------------
# C012 diagnostics
# ---------------------------------------------------------------------------
def _expect_c012(src: str) -> None:
    reg, _ = _registries({}, "build_bad", "CB")
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
    build    build_bad
    entry    @s

    op S @s -> op P -> $hop
}}
"""


def test_c012_capture_inside_cyclic_body():
    _expect_c012(_BAD_TEMPLATE.format(
        rule_body="op S -> op P @cap -> ( op T -> $hop | op F )"))


def test_c012_self_ref_not_chain_tail():
    _expect_c012(_BAD_TEMPLATE.format(
        rule_body="op S -> op P -> ( op T -> $hop -> op F | op F )"))


def test_c012_maybe_self_ref_in_body():
    _expect_c012(_BAD_TEMPLATE.format(
        rule_body="op S -> op P -> ( op T -> maybe $hop | op F )"))


def test_c012_other_recursive_rule_in_body():
    src = """
    opcode S = 0x1
    opcode P = 0x2
    opcode T = 0x3
    opcode F = 0x4

    rule other = ( op T -> maybe $other | op F )

    rule hop cyclic = (
        op S -> op P -> ( op T -> $hop
                        | op T -> $other
                        | op F )
    )

    matcher bad {
        produces CB
        build    build_bad
        entry    @s

        op S @s -> op P -> $hop
    }
    """
    _expect_c012(src)


# ---------------------------------------------------------------------------
# IR loader validation
# ---------------------------------------------------------------------------
def _cyclic_artifact():
    compiled, _reg, _c = _compile_cyclic()
    return emit_ir(compiled, ["cyc.mch"])


def _rec_nodes(art):
    return [n for n in art["nodes"] if n.get("kind") == "recursive"]


def test_loader_rejects_unknown_strategy():
    art = _cyclic_artifact()
    _rec_nodes(art)[0]["strategy"] = "spiral"
    with pytest.raises(IrLoadError, match="unknown recursion strategy"):
        mch_ir.load_artifact(art)


def test_loader_rejects_cyclic_with_exit():
    art = _cyclic_artifact()
    rec = _rec_nodes(art)[0]
    rec["exit"] = rec["step"]
    with pytest.raises(IrLoadError, match="cannot carry an exit"):
        mch_ir.load_artifact(art)


def test_loader_rejects_capture_inside_cyclic_body():
    art = _cyclic_artifact()
    rec = _rec_nodes(art)[0]
    body = art["nodes"][rec["step"]]
    body["capture"] = "smuggled"
    for m in art["matchers"]:
        m.setdefault("captures", []).append({"name": "smuggled", "card": "one"})
    loaded = mch_ir.load_artifact(art)
    reg, _ = _registries({}, "build_cyc", "CB")
    with pytest.raises(IrLoadError, match="captures inside a cyclic"):
        mch_ir.build_matchers(loaded, reg)


# ---------------------------------------------------------------------------
# The T28 "maybe inside an alternative" quirk — pinned
# ---------------------------------------------------------------------------
@pytest.mark.asyncio
async def test_legacy_or_matcher_ignores_branch_optionality(build_block_tree):
    """Legacy pin: OrMatcher.try_build never consults a branch's `optional`
    flag — a losing optional branch falls through, and if every branch loses
    the Or FAILS rather than succeeding empty. ToncoSwapBlockMatcher's final
    branches carry optional=True and rely on exactly this inertness."""
    or_m = OrMatcher([
        ContractMatcher(opcode=0xAA, optional=True),
        ContractMatcher(opcode=0xBB),
    ])
    non_matching = build_block_tree(BlockSpec(opcode=0xCC))
    assert await or_m.try_build(non_matching) is None  # not [] — no empty success
    matching_second = build_block_tree(BlockSpec(opcode=0xBB))
    assert await or_m.try_build(matching_second) is not None


@pytest.mark.asyncio
async def test_mch_maybe_branch_inside_alternative_is_inert(build_block_tree):
    """mch pin, both engines: `( maybe op X | op Y )` as a required child —
    branch-level `maybe` does NOT make the alternative optional. A tree whose
    child matches neither branch fails the match; the losing maybe-branch
    contributes nothing."""
    src = """
    opcode A = 0x1
    opcode X = 0x2
    opcode Y = 0x3

    matcher quirk {
        produces QB
        build    build_quirk
        entry    @a

        op A @a -> ( maybe op X | op Y )
    }
    """
    for factory in ("compiled", "ir"):
        reg, captured = _registries({}, "build_quirk", "QB")
        compiled = compile_text(src, "quirk.mch", reg)
        if factory == "compiled":
            m = compiled[0].matcher
            use_captured = captured
        else:
            art = mch_ir.load_artifact(emit_ir(compiled, ["quirk.mch"]))
            reg2, use_captured = _registries({}, "build_quirk", "QB")
            m = {x.name: x for x in mch_ir.build_matchers(art, reg2)}["quirk"]

        neither = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x9)]))
        result = await m.try_build(neither)
        assert not result and len(use_captured) == 0, factory

        with_y = build_block_tree(BlockSpec(opcode=0x1, children=[BlockSpec(opcode=0x3)]))
        result = await m.try_build(with_y)
        assert result is not None and len(use_captured) == 1, factory
