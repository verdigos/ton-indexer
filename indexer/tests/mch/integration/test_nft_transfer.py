"""Base nft_transfer remake — parity (legacy == v2 IR) between the
NftTransferBlockMatcher (blocks/nft.py) and specs/nft_transfer.mch.

Assertion depth matches test_getgems / test_telegram_nft: block class + btype,
`data` equality, failed/broken flags, value_flow, consumed event-node set, AND
block topology (children_blocks / next_blocks multisets) — the M2R remake gate
binds on topology because the action hierarchy derives from it, and here the
shaper mutates topology by absorbing the funding parent in the purchase branch.

nft_transfer is a BASE matcher (anchors on op NftTransfer directly), so it runs
with no prerequisite. The corpus:
  * plain transfers (tonstakers fixture, is_purchase false — the shaper no-ops);
  * getgems purchases (nft.yaml fixtures, is_purchase true — the shaper absorbs
    the funding parent, the same block the legacy build merged).
The fragment/telegram purchase (aMUa) is produced by the SEPARATE
TelegramNftPurchaseBlockMatcher, not this base matcher, so it is out of scope
here (covered by test_telegram_nft).
"""
from __future__ import annotations

from pathlib import Path

import pytest

from indexer.events import context
from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.nft import (
    NftTransferBlock,
    NftTransferBlockMatcher,
)
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events import mch_ir
from indexer.events.mch import compile_file
from indexer.events.mch.builders.nft_transfer import nft_transfer_registries
from indexer.events.mch.ir_emit import emit_ir, render_json
import indexer.events.mch.specs as specs_pkg
from tests.utils.repository import TestInterfaceRepository
from tests.utils.trace_deserializer import load_trace_from_file

NFT_TRANSFER_MCH = Path(specs_pkg.__file__).parent / "nft_transfer.mch"
MATCHER_NAME = "nft_transfer"

# (fixture, id) — getgems purchase corpus (nft.yaml). These exercise both the
# plain data build AND the is_purchase branch + funding-parent shaper. A base
# NON-purchase nft_transfer that the matcher fires on standalone (no prereq) is
# not present in the trace corpus (tonstakers' nft_transfer is a pipeline-nested
# case); recording a dedicated one needs live-DB access, out of scope here.
CASES = [
    ("mG5NGg-37hpz1Vgs5KvDo0HKgtQKCnMbmGEpdApY5Ls=.lz4", "getgems-purchase"),
    ("Mk9O4OZz5p1IH604tukwy07EHSY9Tdvwme1kH5iZqek=.lz4", "getgems-auction-purchase"),
    ("F73aJsg-E-RHh3nJel9-KZ23sQ80dLMqt26gbUEXHIs=.lz4", "getgems-auction-purchase-2"),
]


def _load_trace(traces_dir: Path, filename: str):
    trace, interfaces = load_trace_from_file(traces_dir / filename)
    context.interface_repository.set(TestInterfaceRepository(interfaces))
    return trace


def _build_block_tree(trace) -> Block:
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_side(matcher: BlockMatcher, trace) -> list[Block]:
    """One tree per side; the base matcher over a full BFS pass (mirrors
    process_event_async), no prerequisite."""
    root = _build_block_tree(trace)
    produced: list[Block] = []
    for b in root.bfs_iter():
        if b.btype == "root" or b.parent is not None:
            continue
        result = await matcher.try_build(b)
        if result is not None:
            produced.extend(result)
    # Wave-B: the hybrid build produces a plain Block with btype 'nft_transfer'
    # (not a NftTransferBlock instance); filter on btype, not class identity.
    return [b for b in produced if b.btype == "nft_transfer"]


def _node_hashes(block: Block) -> list[str]:
    return sorted(n.message.tx_hash for n in block.event_nodes)


def _sort_key(block: Block):
    return (block.min_lt, tuple(_node_hashes(block)))


def _topology(block: Block) -> tuple[list, list]:
    def sig(bs):
        return sorted((b.btype, tuple(_node_hashes(b))) for b in bs)
    return sig(block.children_blocks), sig(block.next_blocks)


def _assert_parity(actual: list[Block], expected: list[Block],
                   actual_tag: str, expected_tag: str) -> None:
    assert len(expected) > 0, f"{expected_tag} produced no nft_transfer blocks - bad fixture"
    assert len(actual) == len(expected), (
        f"count mismatch: {actual_tag}={len(actual)} {expected_tag}={len(expected)}"
    )
    for a, e in zip(sorted(actual, key=_sort_key), sorted(expected, key=_sort_key)):
        # Wave-B: IR hybrid produces a plain Block(dict); legacy a NftTransferBlock.
        # Assert btype + per-key data, not class identity.
        assert a.btype == e.btype == "nft_transfer"
        assert a.failed == e.failed, f"failed flag: {actual_tag}={a.failed} {expected_tag}={e.failed}"
        assert a.broken == e.broken, f"broken flag: {actual_tag}={a.broken} {expected_tag}={e.broken}"

        # nft_transfer `data` is a plain dict (not a dataclass).
        assert set(a.data.keys()) == set(e.data.keys()), (
            f"data keys: {actual_tag}={sorted(a.data)} {expected_tag}={sorted(e.data)}"
        )
        for key in sorted(e.data.keys()):
            av, ev = a.data[key], e.data[key]
            assert av == ev, (
                f"data.{key}: {actual_tag}={av!r} {expected_tag}={ev!r}"
            )

        assert a.value_flow.to_dict() == e.value_flow.to_dict(), (
            f"value_flow: {actual_tag}={a.value_flow.to_dict()} {expected_tag}={e.value_flow.to_dict()}"
        )
        assert _node_hashes(a) == _node_hashes(e), (
            f"consumed event nodes: {actual_tag}={_node_hashes(a)} {expected_tag}={_node_hashes(e)}"
        )
        assert _topology(a) == _topology(e), (
            f"block topology: {actual_tag}={_topology(a)} {expected_tag}={_topology(e)}"
        )


def _ir_matcher():
    # Compiling is a frontend-internal step ir_emit needs; the IR engine is the
    # DSL runtime (T29), no compiled matcher is run.
    compiled = compile_file(NFT_TRANSFER_MCH, nft_transfer_registries())
    artifact = emit_ir(compiled, [NFT_TRANSFER_MCH.name])
    render_json(artifact)  # emit path must not raise
    art = mch_ir.load_artifact(artifact)
    ir_by_name = {m.name: m for m in mch_ir.build_matchers(art, nft_transfer_registries())}
    return ir_by_name[MATCHER_NAME]


@pytest.mark.asyncio
@pytest.mark.parametrize("filename", [c[0] for c in CASES], ids=[c[1] for c in CASES])
async def test_nft_transfer_parity(traces_dir, filename):
    trace = _load_trace(traces_dir, filename)
    ir_matcher = _ir_matcher()

    hw = await _run_side(NftTransferBlockMatcher(), trace)
    ir = await _run_side(ir_matcher, trace)

    assert len(hw) > 0, "legacy matcher produced no nft_transfer blocks - stale fixture"
    _assert_parity(ir, hw, "ir", "legacy")
