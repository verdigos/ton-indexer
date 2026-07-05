"""R09 — multi-kind anchors + per-branch `where`, through the full IR chain.

Each spec is frontend-compiled, emitted to IR (asserting the `mixed` anchor
record), loaded, and wired by mch_ir. The IR matcher's anchor `test_self` is
then exercised on synthetic blocks; a where-gated branch must reject.
"""
from __future__ import annotations

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block
from indexer.events.mch.compile import compile_text
from indexer.events.mch.ir_emit import emit_ir
from indexer.events.mch.registry import Registries
from indexer.events.mch_ir import build_matchers, load_artifact


def _cc(opcode: int, marked: bool = False) -> CallContractBlock:
    b = CallContractBlock.__new__(CallContractBlock)
    Block.__init__(b, "call_contract", [], None)
    b.opcode = opcode
    b.next_blocks = []
    b.previous_block = None
    b._marked = marked
    return b


def _bt(btype: str, marked: bool = False) -> Block:
    b = Block(btype, [], None)
    b._marked = marked
    return b


def _registries(**opcodes: int) -> Registries:
    reg = Registries()
    reg.opcodes.update(opcodes)
    reg.block_types["MB"] = object
    reg.predicates["is_marked"] = lambda b: bool(getattr(b, "_marked", False))

    @reg.register_builder("build_m")
    async def _build(m):  # noqa: ARG001
        return None
    return reg


def _both(src: str, reg: Registries):
    """(ir_matcher, ir_artifact) from one spec + registries."""
    compiled = compile_text(src, "test.mch", reg)
    artifact = emit_ir(compiled, ["test.mch"])
    art = load_artifact(artifact)
    ir = build_matchers(art, reg)[0]
    return ir, artifact


_PER_BRANCH_WHERE_SRC = """
opcode A = 0x1
opcode B = 0x2
matcher m {
    produces MB
    build    build_m
    entry    @x
    (op A @x ^ op B @x where is_marked)
}
"""

_MIXED_WHERE_SRC = """
opcode A = 0x1
matcher m {
    produces MB
    build    build_m
    entry    @x
    (op A @x where is_marked ^ btype jetton_transfer @x)
}
"""


def test_r09_per_branch_where_emits_mixed_and_matches_both_engines():
    reg = _registries(A=0x1, B=0x2)
    ir, artifact = _both(_PER_BRANCH_WHERE_SRC, reg)

    # Emitted anchor is the `mixed` branch form; the where-carrying branch keeps
    # its predicate name.
    assert artifact["matchers"][0]["anchor"] == {
        "kind": "mixed",
        "branches": [{"op": 0x1}, {"op": 0x2, "where": "is_marked"}],
    }

    cases = [
        _cc(0x1),                 # A branch, no where -> True
        _cc(0x2, marked=True),    # B branch, where passes -> True
        _cc(0x2, marked=False),   # B branch, where fails -> False
        _cc(0x99),                # off-membership -> False
        _bt("jetton_transfer"),   # wrong kind entirely -> False
    ]
    assert [ir.test_self(b) for b in cases] == [True, True, False, False, False]


def test_r09_mixed_op_btype_with_where_both_engines():
    reg = _registries(A=0x1)
    ir, artifact = _both(_MIXED_WHERE_SRC, reg)

    assert artifact["matchers"][0]["anchor"] == {
        "kind": "mixed",
        "branches": [{"op": 0x1, "where": "is_marked"}, {"btype": "jetton_transfer"}],
    }

    cases = [
        _cc(0x1, marked=True),    # op branch, where passes -> True
        _cc(0x1, marked=False),   # op branch, where fails -> False
        _bt("jetton_transfer"),   # btype branch, ungated -> True
        _bt("other"),             # wrong btype -> False
        _cc(0x99),                # off-membership -> False
    ]
    assert [ir.test_self(b) for b in cases] == [True, False, True, False, False]


def test_r09_pure_single_kind_group_unchanged():
    """A pure single-kind XOR with no branch `where` still emits the T10
    `opcode_set` form (regression guard for 'keep old kinds unchanged')."""
    reg = _registries(A=0x1, B=0x2)
    _, artifact = _both(
        """
        opcode A = 0x1
        opcode B = 0x2
        matcher m {
            produces MB
            build    build_m
            entry    @x
            (op A @x ^ op B @x)
        }
        """,
        reg,
    )
    assert artifact["matchers"][0]["anchor"] == {
        "kind": "opcode_set",
        "values": [0x1, 0x2],
    }
