"""Field-level parity between VestingSendMessageBlockMatcher and
specs/vesting.mch, PLUS the IR round trip.

Three matcher graphs are compared block-for-block at the same bar as the T13-T17
ports (btype, deep `data` equality, failed/broken flags, value_flow, consumed
event-node set — count parity alone is NOT acceptance):

    legacy    =  VestingSendMessageBlockMatcher (hand-written)
    compiled  =  compile_file(vesting.mch)                       (frontend)
    ir        =  compile_file -> emit_ir -> load_artifact
                   -> build_matchers                             (v2 IR engine)

legacy == compiled proves the port; compiled == ir e2e-tests the IR engine over
this spec. The anchor is the raw Jetton...-style call chain (owner -> vesting
send-message request + its response), so no prerequisite matchers are needed.

Fixture: 03RZCW7mu3eJp-FWifVzKsRHafUrqQNu4E6YwZxvXJ0= (vesting.yaml "Vesting send
message"). No new recording.
"""
from __future__ import annotations

import dataclasses
from pathlib import Path

import pytest

from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.blocks.vesting import VestingSendMessageBlockMatcher
from indexer.events.event_processing import init_block
from indexer.events import mch_ir
from indexer.events.mch import compile_file
from indexer.events.mch.builders.vesting import vesting_registries
from indexer.events.mch.ir_emit import emit_ir
import indexer.events.mch.specs as specs_pkg

VESTING_MCH = Path(specs_pkg.__file__).parent / "vesting.mch"
MATCHER_NAME = "vesting_send_message"
TARGET_BTYPE = "vesting_send_message"


def _compiled_and_ir() -> tuple[BlockMatcher, BlockMatcher]:
    compiled = compile_file(VESTING_MCH, vesting_registries())
    compiled_by_name = {cm.name: cm.matcher for cm in compiled}
    artifact = emit_ir(compiled, [VESTING_MCH.name])
    art = mch_ir.load_artifact(artifact)
    ir_by_name = {m.name: m for m in mch_ir.build_matchers(art, vesting_registries())}
    return compiled_by_name[MATCHER_NAME], ir_by_name[MATCHER_NAME]


def _build_block_tree(trace) -> Block:
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matcher(matcher: BlockMatcher, trace, prereqs=()) -> list:
    root = _build_block_tree(trace)
    for pre in prereqs:
        for b in root.bfs_iter():
            if b.parent is None:
                await pre.try_build(b)
    matches: list = []
    for b in root.bfs_iter():
        if b.btype == "root" or b.parent is not None:
            continue
        result = await matcher.try_build(b)
        if result is not None:
            matches.extend(result)
    return [b for b in matches if b.btype == TARGET_BTYPE]


def _values_equal(a, b) -> bool:
    if a is None and b is None:
        return True
    if type(a) is not type(b):
        return False
    return bool(a == b)


def _node_hashes(block: Block) -> list[str]:
    return sorted(n.message.tx_hash for n in block.event_nodes)


def _sort_key(block: Block):
    return (block.min_lt, tuple(_node_hashes(block)))


def _data_items(block: Block) -> dict:
    """Normalize a block's `data` (dataclass for these staking/vesting blocks,
    dict for others) into a name -> value mapping for field-level comparison."""
    data = block.data
    if dataclasses.is_dataclass(data):
        return {f.name: getattr(data, f.name) for f in dataclasses.fields(data)}
    if isinstance(data, dict):
        return dict(data)
    return dict(vars(data))


def _assert_field_parity(actual: list[Block], reference: list[Block], label: str) -> None:
    assert len(reference) > 0, "reference produced no vesting_send_message blocks - bad fixture"
    assert len(actual) == len(reference), (
        f"{label} count mismatch: actual={len(actual)} reference={len(reference)}"
    )
    for a, r in zip(sorted(actual, key=_sort_key), sorted(reference, key=_sort_key)):
        assert a.btype == r.btype == TARGET_BTYPE
        assert a.failed == r.failed, f"{label} failed flag: {a.failed} vs {r.failed}"
        assert a.broken == r.broken, f"{label} broken flag: {a.broken} vs {r.broken}"
        ad, rd = _data_items(a), _data_items(r)
        assert set(ad.keys()) == set(rd.keys()), (
            f"{label} data keys: {sorted(ad)} vs {sorted(rd)}"
        )
        for key in sorted(rd):
            av, rv = ad[key], rd[key]
            assert _values_equal(av, rv), (
                f"{label} data[{key!r}]: {av!r} ({type(av).__name__}) "
                f"vs {rv!r} ({type(rv).__name__})"
            )
        assert a.value_flow.to_dict() == r.value_flow.to_dict(), (
            f"{label} value_flow: {a.value_flow.to_dict()} vs {r.value_flow.to_dict()}"
        )
        assert _node_hashes(a) == _node_hashes(r), (
            f"{label} consumed nodes: {_node_hashes(a)} vs {_node_hashes(r)}"
        )


@pytest.mark.asyncio
@pytest.mark.trace_file("03RZCW7mu3eJp-FWifVzKsRHafUrqQNu4E6YwZxvXJ0=.lz4")
async def test_vesting_send_message_parity(trace_data):
    """Send-message request + response: legacy == compiled == IR, field-level."""
    trace, _ = trace_data
    compiled_matcher, ir_matcher = _compiled_and_ir()

    hw_blocks = await _run_matcher(VestingSendMessageBlockMatcher(), trace)
    dsl_blocks = await _run_matcher(compiled_matcher, trace)
    ir_blocks = await _run_matcher(ir_matcher, trace)

    _assert_field_parity(dsl_blocks, hw_blocks, "compiled-vs-legacy")
    _assert_field_parity(ir_blocks, dsl_blocks, "ir-vs-compiled")

    # Fixture sanity: the response was consumed alongside the request.
    assert len(hw_blocks[0].event_nodes) >= 2, "fixture no longer carries the response"
