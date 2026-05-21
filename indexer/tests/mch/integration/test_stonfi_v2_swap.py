from __future__ import annotations

import pytest

from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.jettons import (
    JettonTransferBlockMatcher,
    PTonTransferMatcher,
)
from indexer.events.blocks.swaps import (
    JettonSwapBlock,
    StonfiV2SwapBlockMatcher,
)
from indexer.events.blocks.messages import (
    JettonInternalTransfer,
    JettonNotify,
    PTonTransfer,
)
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_text
from indexer.events.mch.registry import Registries


# Mirror of `tests/mch/integration/test_jetton_transfer.py`. This test exercises
# the compiler's RECURSION lowering path: `next_hops` is a recursive rule
# referencing itself, which lowers to a `RecursiveMatcher`.
#
# Note: the hand-written `StonfiV2SwapBlockMatcher` is structurally far richer
# than what we can express in v0 (e.g. it threads `peer_swap`/`payout` labels,
# does pton validation in a custom GenericMatcher, etc.). Our DSL pattern is a
# necessary simplification - the goal is to verify that the recursion lowering
# fires at least as often as the hand-written matcher on a known-good trace.

STONFI_V2_SWAP_MCH = """
opcode StonfiV2Swap           = 0x6664de2a
opcode StonfiV2PayTo          = 0x657b54f5
opcode JettonNotify           = 0x7362d09c
opcode PTonTransfer           = 0x01f3835d
opcode JettonInternalTransfer = 0x178d4519

predicate pton_self_transfer

rule next_hops = (
      op JettonInternalTransfer where pton_self_transfer -> op JettonNotify -> maybe $next_hops
    | btype jetton_transfer -> maybe $next_hops
    | maybe $next_hops
)

matcher stonfi_v2_swap_dsl {
    produces JettonSwapBlock
    build    build_stonfi_v2_swap_dsl
    entry    @swap

    op StonfiV2Swap @swap
      -> op StonfiV2PayTo @payout
      -> $next_hops
}
"""


def _make_registries(captured_anchors: list | None = None) -> Registries:
    reg = Registries()
    reg.opcodes.update({
        "StonfiV2Swap":           0x6664de2a,
        "StonfiV2PayTo":          0x657b54f5,
        "JettonNotify":           JettonNotify.opcode,
        "PTonTransfer":           PTonTransfer.opcode,
        "JettonInternalTransfer": JettonInternalTransfer.opcode,
    })
    reg.block_types["JettonSwapBlock"] = JettonSwapBlock

    # Stub pton_self_transfer — for the test we allow any internal transfer.
    @reg.register_predicate("pton_self_transfer")
    def pton_self_transfer(block) -> bool:
        return True

    @reg.register_builder("build_stonfi_v2_swap_dsl")
    async def build_stonfi_v2_swap_dsl(m):
        # Phase 1 capture extraction: m.swap and m.payout are outside the recursive
        # rule and must bind. Captures inside `next_hops` (none today) would be None
        # under Phase 1 — that's Phase 3's job.
        assert m.swap is not None, "anchor capture must be populated"
        assert getattr(m.swap, "opcode", None) == 0x6664de2a
        assert m.payout is not None, "payout capture must be populated"
        assert getattr(m.payout, "opcode", None) == 0x657b54f5
        if captured_anchors is not None:
            captured_anchors.append(m.swap)
        return JettonSwapBlock({})

    return reg


def _build_block_tree(trace) -> Block:
    """Replicate event_processing's prelude: tree of EventNodes -> tree of Blocks."""
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


# Prerequisite matchers that must run before any DEX swap matcher. In the
# production `matchers` list (event_processing.py) these run earlier so that
# downstream swap matchers can pattern-match on `btype == 'jetton_transfer'`
# and on PTon-transfer-shaped blocks. We must mirror that ordering here for
# both the DSL and the hand-written matcher to have a fair shot.
_PREREQUISITES: list[BlockMatcher] = [
    JettonTransferBlockMatcher(),
    PTonTransferMatcher(),
]


async def _apply_matchers(root: Block, matchers: list[BlockMatcher]) -> None:
    """Mirror event_processing.process_event_async's matcher loop."""
    for m in matchers:
        for b in root.bfs_iter():
            if b.parent is None:
                await m.try_build(b)


async def _run_matcher(matcher: BlockMatcher, trace) -> list:
    """Build the block tree, run prerequisites, then collect this matcher's results.

    We can't simply count blocks of the produced type after `try_build` because
    `try_build` mutates the tree in-place (sets parent links, absorbs children).
    Instead we collect the result blocks returned by `try_build` calls on the
    root-level blocks - this is what `process_event_async` ultimately uses to
    serialise actions.
    """
    root = _build_block_tree(trace)
    await _apply_matchers(root, _PREREQUISITES)

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


@pytest.mark.asyncio
@pytest.mark.trace_file("vdNpUDA43aIw6QlbcKMgi7q4S0MnJAVTfXVRqbAt5Tw=.lz4")
async def test_dsl_matches_hand_written_stonfi_v2_swap(trace_data):
    trace, _ = trace_data

    captured_anchors: list = []
    reg = _make_registries(captured_anchors)
    compiled = compile_text(STONFI_V2_SWAP_MCH, "stonfi_v2_swap.mch", reg)
    dsl_matcher = compiled[0].matcher

    dsl_results = await _run_matcher(dsl_matcher, trace)
    hw_results = await _run_matcher(StonfiV2SwapBlockMatcher(), trace)

    dsl_swaps = [b for b in dsl_results if isinstance(b, JettonSwapBlock)]
    hw_swaps = [b for b in hw_results if isinstance(b, JettonSwapBlock)]

    print(f"\n[stonfi-v2-swap] DSL={len(dsl_swaps)} HW={len(hw_swaps)}")
    assert len(hw_swaps) > 0, "hand-written stonfi v2 matcher found no swaps - bad fixture"
    assert len(dsl_swaps) == len(hw_swaps), (
        f"DSL matched {len(dsl_swaps)} swaps, hand-written matched {len(hw_swaps)}"
    )

    # Phase 2 absorption check.
    assert len(captured_anchors) >= 1
    for anchor in captured_anchors:
        assert anchor.parent is not None, (
            "Phase 2: merge_blocks must wire anchor.parent to the produced block"
        )
