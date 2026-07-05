"""Field-level parity between EthenaDepositBlockMatcher and specs/ethena.mch,
PLUS the IR round trip.

Three matcher graphs are compared block-for-block at the T13-T17 bar (btype,
deep `data` equality, failed/broken flags, value_flow, consumed event-node set):

    legacy    =  EthenaDepositBlockMatcher (hand-written)
    compiled  =  compile_file(ethena.mch)                        (frontend)
    ir        =  compile_file -> emit_ir -> load_artifact
                   -> build_matchers                             (v2 IR engine)

ethena.mch is the FIRST btype-anchored spec (`btype jetton_transfer` anchor), so
this round trip also exercises the IR btype-set anchor path end to end.

Prerequisites: the matcher anchors on an already-built `jetton_transfer` block
and its `jetton_mint` child, so JettonTransfer + JettonMint matchers prepare the
tree first (production order: both run before EthenaDepositBlockMatcher). They
run identically on all three paths, so they are neutral tree preparation.

Fixture: vhM4-FbBKgC8khEg0MTKMvPx8MiBuEYbrqHgDjPFHcQ= (ethena.yaml "Deposit"). No
new recording.
"""
from __future__ import annotations

import dataclasses
from pathlib import Path

import pytest

from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.ethena_staking import EthenaDepositBlockMatcher
from indexer.events.blocks.jettons import (
    JettonMintBlockMatcher,
    JettonTransferBlockMatcher,
)
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events import mch_ir
from indexer.events.mch import compile_file
from indexer.events.mch.builders.ethena import ethena_registries
from indexer.events.mch.ir_emit import emit_ir
import indexer.events.mch.specs as specs_pkg

ETHENA_MCH = Path(specs_pkg.__file__).parent / "ethena.mch"
MATCHER_NAME = "ethena_deposit"
TARGET_BTYPE = "ethena_deposit"


def _prerequisites() -> list[BlockMatcher]:
    # jetton_transfer anchor + jetton_mint child must exist before the deposit
    # matcher runs (production registration order).
    return [JettonTransferBlockMatcher(), JettonMintBlockMatcher()]


def _ir_matcher() -> BlockMatcher:
    # Compiling is a frontend-internal step ir_emit needs; the compiled matcher
    # is never run as a runtime (T29).
    compiled = compile_file(ETHENA_MCH, ethena_registries())
    artifact = emit_ir(compiled, [ETHENA_MCH.name])
    art = mch_ir.load_artifact(artifact)
    ir_by_name = {m.name: m for m in mch_ir.build_matchers(art, ethena_registries())}
    return ir_by_name[MATCHER_NAME]


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
    data = block.data
    if dataclasses.is_dataclass(data):
        return {f.name: getattr(data, f.name) for f in dataclasses.fields(data)}
    if isinstance(data, dict):
        return dict(data)
    return dict(vars(data))


def _assert_field_parity(actual: list[Block], reference: list[Block], label: str) -> None:
    assert len(reference) > 0, "reference produced no ethena_deposit blocks - bad fixture"
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
@pytest.mark.trace_file("vhM4-FbBKgC8khEg0MTKMvPx8MiBuEYbrqHgDjPFHcQ=.lz4")
async def test_ethena_deposit_parity(trace_data):
    """USDe -> tsUSDe deposit: legacy == compiled == IR, field-level, including
    the hardcoded-asset gate passing and the btype anchor."""
    trace, _ = trace_data
    ir_matcher = _ir_matcher()

    hw_blocks = await _run_matcher(EthenaDepositBlockMatcher(), trace, _prerequisites())
    ir_blocks = await _run_matcher(ir_matcher, trace, _prerequisites())

    _assert_field_parity(ir_blocks, hw_blocks, "ir-vs-legacy")
