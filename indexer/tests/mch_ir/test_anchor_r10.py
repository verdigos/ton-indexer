"""R10 — predicate anchors, through the full IR chain.

A `pred`-headed `entry` compiles to an anchor with NO opcode/btype prefilter:
`test_self` is the predicate itself, full-scanned over every candidate block.
The spec is frontend-compiled, emitted (asserting the `pred` anchor record),
loaded and wired; the IR matcher's behavior is compared against the compiled
matcher (compiled == IR) on synthetic blocks.
"""
from __future__ import annotations

import pytest

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
    b.next_blocks = []
    b.previous_block = None
    b._marked = marked
    return b


_PRED_SRC = """
matcher m {
    produces MB
    build    build_m
    entry    @x
    pred is_marked @x
}
"""


def _registries():
    reg = Registries()
    reg.block_types["MB"] = object
    reg.predicates["is_marked"] = lambda b: bool(getattr(b, "_marked", False))
    produced_seen: list = []

    @reg.register_builder("build_m")
    async def _build(m):  # noqa: ARG001
        produced_seen.append(m)
        return Block("MB", [], {"ok": True})
    return reg, produced_seen


def _both(reg):
    compiled = compile_text(_PRED_SRC, "test.mch", reg)
    artifact = emit_ir(compiled, ["test.mch"])
    ir = build_matchers(load_artifact(artifact), reg)[0]
    return compiled[0].matcher, ir, artifact


def test_r10_pred_anchor_emits_pred_record_and_matches_both_engines():
    reg, _ = _registries()
    compiled, ir, artifact = _both(reg)

    # No prefilter kind — the anchor record names the predicate.
    assert artifact["matchers"][0]["anchor"] == {"kind": "pred", "pred": "is_marked"}

    # Full-scan test_self: fires on ANY block the predicate accepts, regardless
    # of opcode/btype; compiled and IR agree.
    cases = [
        _cc(0x1, marked=True),
        _cc(0x1, marked=False),
        _bt("jetton_transfer", marked=True),
        _bt("anything", marked=False),
    ]
    for blk in cases:
        assert compiled.test_self(blk) == ir.test_self(blk), blk
    assert [ir.test_self(b) for b in cases] == [True, False, True, False]


@pytest.mark.asyncio
async def test_r10_pred_anchor_gates_try_build_both_engines():
    # The pred anchor gates try_build identically on both engines: an unmarked
    # block is a structural mismatch (test_self False) and never reaches the
    # build phase. (The build phase itself is unchanged by R10 and is exercised
    # with full fake event-node fixtures in test_engine.py.)
    reg, seen = _registries()
    compiled, ir, _ = _both(reg)

    unmarked = _cc(0x1, marked=False)
    assert await compiled.try_build(unmarked) is None
    assert await ir.try_build(unmarked) is None
    assert seen == []  # builder never invoked
