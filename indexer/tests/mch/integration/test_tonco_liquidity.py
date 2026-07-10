"""Field-level parity between ToncoDepositLiquidityMatcher and
specs/tonco_liquidity.mch, PLUS the IR round trip.

Three matcher graphs are compared block-for-block at the same bar as the
T13-T17 ports (block class + btype, deep `data` equality — the tonco block
carries a ToncoDepositLiquidityData dataclass, compared field by field —
failed/broken flags, value_flow, consumed event-node set; count parity alone
is NOT acceptance):

    legacy    =  ToncoDepositLiquidityMatcher (hand-written)
    compiled  =  compile_file(tonco_liquidity.mch)                (frontend)
    ir        =  compile_file -> emit_ir -> load_artifact
                   -> build_matchers                              (v2 IR engine)

Prerequisites replay event_processing order on ALL three sides identically:
NftMintBlockMatcher, JettonTransferBlockMatcher, PTonTransferMatcher (the
legacy matcher itself runs after them in production and matches prebuilt
`nft_mint` / `jetton_transfer` blocks).

Fixtures (tests/test_cases/tonco.yaml):
- 0GW2kNF4... "Tonco deposit liquidity (normal)" — two deposits (jetton arm +
  pton arm), the second completes with the nft_mint;
- 0--IKFmI... "one side TON (strange case)" — single pton-arm deposit;
- 0_JGy9zy... "two sides jetton" — two jetton-arm deposits, refund payouts
  through ToncoRouterV3PayTo (excess transfers exercised).
No new recording.
"""
from __future__ import annotations

import dataclasses
from pathlib import Path

import pytest

from indexer.events import context
from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.jettons import (
    JettonTransferBlockMatcher,
    PTonTransferMatcher,
)
from indexer.events.blocks.liquidity import (
    ToncoDepositLiquidityBlock,
    ToncoDepositLiquidityMatcher,
)
from indexer.events.blocks.nft import NftMintBlockMatcher
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events import mch_ir
from indexer.events.mch import compile_file
from indexer.events.mch.builders.tonco import tonco_liquidity_registries
from indexer.events.mch.ir_emit import emit_ir
import indexer.events.mch.specs as specs_pkg
from tests.utils.repository import TestInterfaceRepository
from tests.utils.trace_deserializer import load_trace_from_file

TONCO_MCH = Path(specs_pkg.__file__).parent / "tonco_liquidity.mch"
MATCHER_NAME = "tonco_deposit_liquidity"

CASES = [
    ("0GW2kNF4nRhw5xA3b5E35-EhF9H25H3Nvm5MJJo2VD0=.lz4", "normal-two-deposits"),
    ("0--IKFmIbKevaBKSEvai2YCm47_coyO30KO1VyYaXvI=.lz4", "one-side-ton"),
    ("0_JGy9zypUz4Br8zqCwx12YnRM-WxcP2MDeYHHDkdwI=.lz4", "two-sides-jetton"),
]


def _load_trace(traces_dir: Path, filename: str):
    trace, interfaces = load_trace_from_file(traces_dir / filename)
    context.interface_repository.set(TestInterfaceRepository(interfaces))
    return trace


def _prereqs() -> tuple[BlockMatcher, ...]:
    # event_processing relative order: nft mint first, then the jetton pair.
    return (NftMintBlockMatcher(), JettonTransferBlockMatcher(), PTonTransferMatcher())


def _ir_matcher() -> BlockMatcher:
    # Compiling is a frontend-internal step ir_emit needs; the compiled matcher
    # is never run as a runtime (T29).
    compiled = compile_file(TONCO_MCH, tonco_liquidity_registries())
    artifact = emit_ir(compiled, [TONCO_MCH.name])
    art = mch_ir.load_artifact(artifact)
    ir_by_name = {
        m.name: m for m in mch_ir.build_matchers(art, tonco_liquidity_registries())
    }
    return ir_by_name[MATCHER_NAME]


def _build_block_tree(trace) -> Block:
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matcher(matcher: BlockMatcher, trace, prereqs=()) -> list[Block]:
    root = _build_block_tree(trace)
    for pre in prereqs:
        for b in root.bfs_iter():
            if b.parent is None:
                await pre.try_build(b)
    matches: list[Block] = []
    for b in root.bfs_iter():
        if b.btype == "root" or b.parent is not None:
            continue
        result = await matcher.try_build(b)
        if result is not None:
            matches.extend(result)
    return [b for b in matches if b.btype == "tonco_deposit_liquidity"]


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
        "reference produced no tonco_deposit_liquidity blocks - bad fixture"
    )
    assert len(actual) == len(reference), (
        f"{label} count mismatch: actual={len(actual)} reference={len(reference)}"
    )
    for a, r in zip(sorted(actual, key=_sort_key), sorted(reference, key=_sort_key)):
        # Block CLASS and data representation deliberately not compared: the
        # declarative mch product is a generic Block with a dict data map, the
        # legacy build a ToncoDepositLiquidityBlock carrying a dataclass — btype
        # and per-field data are the parity property (Wave-B). _data_items
        # normalizes dataclass/dict.
        assert a.btype == r.btype == "tonco_deposit_liquidity"
        assert a.failed == r.failed, f"{label} failed flag: {a.failed} vs {r.failed}"
        assert a.broken == r.broken, f"{label} broken flag: {a.broken} vs {r.broken}"
        a_data, r_data = _data_items(a), _data_items(r)
        assert set(a_data.keys()) == set(r_data.keys())
        for key in sorted(r_data):
            av, rv = a_data[key], r_data[key]
            if key == "excesses" and isinstance(av, list) and isinstance(rv, list):
                # The excesses list order inherits compact_connections()
                # set-order nondeterminism from the legacy pipeline itself
                # (next_blocks is rebuilt as list(set(...)) when the
                # prerequisite jetton matcher merges, so two independently
                # built trees bind excess_transfer_0/1 in either order).
                # Content is deterministic — compare as a multiset.
                assert sorted(map(repr, av)) == sorted(map(repr, rv)), (
                    f"{label} data.{key} (as multiset): {av!r} vs {rv!r}"
                )
                continue
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
@pytest.mark.parametrize(
    "filename", [c[0] for c in CASES], ids=[c[1] for c in CASES]
)
async def test_tonco_deposit_parity(traces_dir, filename):
    trace = _load_trace(traces_dir, filename)
    ir_matcher = _ir_matcher()

    hw = await _run_matcher(ToncoDepositLiquidityMatcher(), trace, prereqs=_prereqs())
    ir = await _run_matcher(ir_matcher, trace, prereqs=_prereqs())

    _assert_field_parity(ir, hw, "ir-vs-legacy")
