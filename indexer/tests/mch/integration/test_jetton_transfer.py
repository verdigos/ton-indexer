from __future__ import annotations

import pytest

from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.jettons import (
    JettonTransferBlock,
    JettonTransferBlockMatcher,
)
from indexer.events.blocks.messages import (
    JettonInternalTransfer,
    JettonNotify,
    JettonTransfer,
)
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_text
from indexer.events.mch.registry import Registries


JETTON_TRANSFER_MCH = """
opcode JettonTransfer         = 0x0f8a7ea5
opcode JettonInternalTransfer = 0x178d4519
opcode JettonNotify           = 0x7362d09c

matcher jetton_transfer_dsl {
    produces JettonTransferBlock
    build    build_jetton_transfer_dsl
    entry    @transfer

    op JettonTransfer @transfer
      -> op JettonInternalTransfer @internal
      -> maybe op JettonNotify @notify
}
"""


def _make_registries(captured_anchors: list | None = None) -> Registries:
    reg = Registries()
    reg.opcodes.update({
        "JettonTransfer":         JettonTransfer.opcode,
        "JettonInternalTransfer": JettonInternalTransfer.opcode,
        "JettonNotify":           JettonNotify.opcode,
    })
    reg.block_types["JettonTransferBlock"] = JettonTransferBlock

    @reg.register_builder("build_jetton_transfer_dsl")
    async def build_jetton_transfer_dsl(m):
        # Phase 1 capture extraction binds named captures positionally; assert that.
        assert m.transfer is not None, "anchor capture must be populated"
        assert getattr(m.transfer, "opcode", None) == JettonTransfer.opcode, (
            f"m.transfer.opcode = {getattr(m.transfer, 'opcode', None)!r}, "
            f"expected {JettonTransfer.opcode!r}"
        )
        assert m.internal is not None, "internal capture must be populated"
        assert getattr(m.internal, "opcode", None) == JettonInternalTransfer.opcode
        # m.notify is `maybe`-wrapped — may legitimately be None on traces with no notify leg.
        if m.notify is not None:
            assert getattr(m.notify, "opcode", None) == JettonNotify.opcode
        if captured_anchors is not None:
            captured_anchors.append(m.transfer)
        return JettonTransferBlock({}, None)

    return reg


def _build_block_tree(trace) -> Block:
    """Replicate event_processing's prelude: tree of EventNodes -> tree of Blocks."""
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matcher(matcher: BlockMatcher, trace) -> list:
    """Run a matcher across the block tree, mirroring event_processing.process_event_async.

    Production code calls `m.try_build(b)` on every block where `b.parent is None`
    (i.e. not yet absorbed by a prior matcher). On a fresh tree all blocks satisfy
    that, so we walk the full BFS, skipping only the synthetic 'root' block.
    """
    root = _build_block_tree(trace)
    matches: list = []
    for b in root.bfs_iter():
        if b.btype == "root":
            continue
        if b.parent is not None:
            # Already absorbed by an earlier matcher in this loop.
            continue
        result = await matcher.try_build(b)
        if result is not None:
            matches.extend(result)
    return matches


# NOTE: The original Task 21 spec called for trace
#   `S4XM4TRqBSWvsrGUQY_NLEbIRM7q7iS6hx-uWg4Hv14=.lz4`
# but that fixture is the "Failed jetton transfers (external, identical
# transfers)" case from `tests/test_cases/jetton-transfer.yaml` - it contains
# only the external-in transaction with no internal jetton transfer txs. In
# the standard pipeline (init_block + to_tree) there are zero JettonTransfer
# call_contract blocks, so neither matcher fires. The YAML test for that
# trace succeeds because the framework uses `try_process_unknown_event` and
# `init_from_external` for failed externals - a different code path the v0
# integration test does not exercise.
#
# We switch to `KYQfjYD1YybzQIuoJwkoXL-Y-apztmw6AGjIcPhotjQ=.lz4` - a
# documented fixture from `tests/test_cases/dedust-liquidities.yaml` that
# contains two successful jetton transfers (with internal_transfer and
# notify legs both present), exercising the structural pattern this matcher
# is designed to recognise.
@pytest.mark.asyncio
@pytest.mark.trace_file("KYQfjYD1YybzQIuoJwkoXL-Y-apztmw6AGjIcPhotjQ=.lz4")
async def test_dsl_matches_hand_written_jetton_transfer(trace_data):
    trace, _ = trace_data

    # Compile the DSL with anchor-capture list.
    captured_anchors: list = []
    reg = _make_registries(captured_anchors)
    compiled = compile_text(JETTON_TRANSFER_MCH, "jetton_transfer.mch", reg)
    dsl_matcher = compiled[0].matcher

    dsl_results = await _run_matcher(dsl_matcher, trace)
    hw_matcher = JettonTransferBlockMatcher()
    hw_results = await _run_matcher(hw_matcher, trace)

    dsl_jt = [b for b in dsl_results if isinstance(b, JettonTransferBlock)]
    hw_jt = [b for b in hw_results if isinstance(b, JettonTransferBlock)]

    assert len(hw_jt) > 0, "hand-written matcher found no jetton transfers - bad fixture"
    assert len(dsl_jt) == len(hw_jt), (
        f"DSL matched {len(dsl_jt)} jetton_transfer blocks, "
        f"hand-written matched {len(hw_jt)}"
    )

    # Phase 2 absorption: every consumed anchor block must have parent set.
    assert len(captured_anchors) >= 1
    for anchor in captured_anchors:
        assert anchor.parent is not None, (
            "Phase 2: merge_blocks must wire anchor.parent to the produced block"
        )
