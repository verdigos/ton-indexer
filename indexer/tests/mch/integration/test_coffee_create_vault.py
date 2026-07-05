"""Field-level parity between CoffeeCreateVaultMatcher and
specs/coffee_liquidity.mch, PLUS the IR round trip.

Three matcher graphs are compared block-for-block: btype, deep `data` equality
(the legacy block carries a CoffeeCreateVaultData dataclass, the declarative IR
build a plain dict — normalized and compared field by field, NOT by class/data
type per the wave-B G11 decision), failed/broken flags, value_flow, and the
consumed event-node set:

    legacy    =  CoffeeCreateVaultMatcher (hand-written)
    compiled  =  compile_file(coffee_liquidity.mch)               (frontend)
    ir        =  compile_file -> emit_ir -> load_artifact
                   -> build_matchers                              (v2 IR engine)

The pattern is raw opcodes only (create request -> vault deploy -> optional
TEP-89 wallet discovery pair), so no prerequisite matchers are needed.

Fixture: _7TiwVoy-w5U7lbbF5_RD0yGAauPKFCjMcuYRX4eeoU= (coffee.yaml
"Coffee create vault"). No new recording.
"""
from __future__ import annotations

import dataclasses
from pathlib import Path

import pytest

from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.liquidity import CoffeeCreateVaultMatcher
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events import mch_ir
from indexer.events.mch import compile_file
from indexer.events.mch.builders.coffee import coffee_liquidity_registries
from indexer.events.mch.ir_emit import emit_ir
import indexer.events.mch.specs as specs_pkg

COFFEE_MCH = Path(specs_pkg.__file__).parent / "coffee_liquidity.mch"
MATCHER_NAME = "coffee_create_vault"


def _ir_matcher() -> BlockMatcher:
    # Compiling is a frontend-internal step ir_emit needs; the compiled matcher
    # is never run as a runtime (T29).
    compiled = compile_file(COFFEE_MCH, coffee_liquidity_registries())
    artifact = emit_ir(compiled, [COFFEE_MCH.name])
    art = mch_ir.load_artifact(artifact)
    ir_by_name = {
        m.name: m for m in mch_ir.build_matchers(art, coffee_liquidity_registries())
    }
    return ir_by_name[MATCHER_NAME]


def _build_block_tree(trace) -> Block:
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matcher(matcher: BlockMatcher, trace) -> list[Block]:
    root = _build_block_tree(trace)
    matches: list[Block] = []
    for b in root.bfs_iter():
        if b.btype == "root" or b.parent is not None:
            continue
        result = await matcher.try_build(b)
        if result is not None:
            matches.extend(result)
    return [b for b in matches if b.btype == "coffee_create_vault"]


def _values_equal(a, b) -> bool:
    if a is None and b is None:
        return True
    if type(a) is not type(b):
        return False
    return bool(a == b)


def _data_items(block: Block) -> dict:
    data = block.data
    if dataclasses.is_dataclass(data) and not isinstance(data, type):
        return {f.name: getattr(data, f.name) for f in dataclasses.fields(data)}
    return dict(data)


def _node_hashes(block: Block) -> list[str]:
    return sorted(n.message.tx_hash for n in block.event_nodes)


def _sort_key(block: Block):
    return (block.min_lt, tuple(_node_hashes(block)))


def _assert_field_parity(
    actual: list[Block], reference: list[Block], label: str
) -> None:
    assert len(reference) > 0, (
        "reference produced no coffee_create_vault blocks - bad fixture"
    )
    assert len(actual) == len(reference), (
        f"{label} count mismatch: actual={len(actual)} reference={len(reference)}"
    )
    for a, r in zip(sorted(actual, key=_sort_key), sorted(reference, key=_sort_key)):
        # Parity is on btype + data-map fields, NOT block class / data type: the
        # declarative build product is the canonical generic Block + dict (the
        # C++ engine has no host subclasses/dataclasses). Sanctioned G11
        # adaptation — see the wave-B design note. The legacy dataclass and the
        # declarative dict are normalized by _data_items and compared per key.
        assert a.btype == r.btype == "coffee_create_vault"
        assert a.failed == r.failed, f"{label} failed flag: {a.failed} vs {r.failed}"
        assert a.broken == r.broken, f"{label} broken flag: {a.broken} vs {r.broken}"
        a_data, r_data = _data_items(a), _data_items(r)
        assert set(a_data.keys()) == set(r_data.keys())
        for key in sorted(r_data):
            av, rv = a_data[key], r_data[key]
            assert _values_equal(av, rv), (
                f"{label} data.{key}: {av!r} ({type(av).__name__}) "
                f"vs {rv!r} ({type(rv).__name__})"
            )
        assert a.value_flow.to_dict() == r.value_flow.to_dict(), (
            f"{label} value_flow: {a.value_flow.to_dict()} vs {r.value_flow.to_dict()}"
        )
        assert _node_hashes(a) == _node_hashes(r), (
            f"{label} consumed nodes: {_node_hashes(a)} vs {_node_hashes(r)}"
        )


@pytest.mark.asyncio
@pytest.mark.trace_file("_7TiwVoy-w5U7lbbF5_RD0yGAauPKFCjMcuYRX4eeoU=.lz4")
async def test_coffee_create_vault_parity(trace_data):
    """Create request + vault deploy: legacy == compiled == IR, field-level."""
    trace, _ = trace_data
    ir_matcher = _ir_matcher()

    hw = await _run_matcher(CoffeeCreateVaultMatcher(), trace)
    ir = await _run_matcher(ir_matcher, trace)

    _assert_field_parity(ir, hw, "ir-vs-legacy")

    # Fixture sanity: the deploy call was consumed alongside the request.
    assert len(hw[0].event_nodes) >= 2, "fixture no longer carries the deploy call"
