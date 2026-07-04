"""Three-way parity for tgBTC mint (roadmap R03): legacy
TgBTCMintBlockMatcher == compiled specs/tgbtc.mch == IR-engine round trip, on
every mint fixture in tests/test_cases/tgbtc.yaml.

Assertion depth matches test_evaa_withdraw / the remake gate: btype, per-field
`data` equality (TgBTCMintData dataclass), failed/broken flags, value_flow,
consumed event-node set, AND block topology (produced block's next_blocks /
children_blocks signature). Count parity alone is not acceptance.

The remake re-anchors on the mint chain HEAD (0x3F781D24) and descends an
`any`-headed recursion to the TAIL (0xE42A879B); the legacy matcher anchors on
the tail and walks `previous_block` back to the head. Both must consume the
identical block set and produce the identical block.

Fixtures (the four tgbtc.yaml mint cases; no new recording):
- iqIHDlbalqobuAMgJOC_T_9L6nLiwdOOr7BNKBtqIyA=  short mint (3 trunk hops).
- 1OrxQbhicqml435pt0GPrXePkkdTGXRMlNJ4q67UZ-M=  short mint, log/mint child
    order swapped.
- 1rMRvEYh8TtCNox3P4VG6wcGLWX4YrU7jc55iqxzo6c=  "very long" chain (11 hops).
- 1qD3P7k62Wscr4J92DXfRmsFUoXsd631PVCjJDuZPi0=  "very long" chain (11 hops).

Prerequisite matchers: JettonTransferBlockMatcher + JettonMintBlockMatcher build
the `jetton_mint` child the tail's children set depends on (identical tree prep
on all three paths).
"""
from __future__ import annotations

from dataclasses import fields
from pathlib import Path

import pytest

from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.jettons import (
    JettonMintBlockMatcher,
    JettonTransferBlockMatcher,
)
from indexer.events.blocks.tgbtc import TgBTCMintBlockMatcher, TgBTCMintData
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_file
from indexer.events.mch.builders.tgbtc import tgbtc_registries
from indexer.events.mch.ir_emit import emit_ir, render_json
import indexer.events.mch.specs as specs_pkg

# --- IR engine gate (same convention as test_evaa_withdraw) -----------------
try:
    from indexer.events import mch_ir as _mch_ir

    _HAVE_IR = hasattr(_mch_ir, "load_artifact") and hasattr(_mch_ir, "build_matchers")
except Exception:  # pragma: no cover - import error path
    _mch_ir = None
    _HAVE_IR = False

_SKIP_REASON = (
    "indexer.events.mch_ir not importable; the legacy-vs-compiled parity and "
    "the IR emit above still ran"
)

TGBTC_MCH = Path(specs_pkg.__file__).parent / "tgbtc.mch"

_PREREQS = [JettonTransferBlockMatcher, JettonMintBlockMatcher]


def _build_block_tree(trace) -> Block:
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matcher(matcher: BlockMatcher, trace) -> list:
    root = _build_block_tree(trace)
    for cls in _PREREQS:
        pre = cls()
        for b in root.bfs_iter():
            if b.parent is None and b.btype != "root":
                await pre.try_build(b)
    matches: list = []
    for b in root.bfs_iter():
        if b.btype == "root" or b.parent is not None:
            continue
        result = await matcher.try_build(b)
        if result is not None:
            matches.extend(result)
    return [b for b in matches if b.btype == "tgbtc_mint"]


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


def _topology_signature(block: Block):
    """Structural fingerprint of the produced block as serialize_blocks sees it:
    the sorted btype/opcode of its next_blocks and children_blocks."""
    def tag(b: Block) -> str:
        op = getattr(b, "opcode", None)
        return f"{b.btype}:{op:#x}" if op is not None else b.btype

    return (
        sorted(tag(c) for c in block.next_blocks),
        sorted(tag(c) for c in block.children_blocks),
    )


def _assert_field_parity(actual: list[Block], expected: list[Block],
                         actual_tag: str, expected_tag: str) -> None:
    assert len(expected) > 0, f"{expected_tag} produced no tgbtc_mint blocks - bad fixture"
    assert len(actual) == len(expected), (
        f"count mismatch: {actual_tag}={len(actual)} {expected_tag}={len(expected)}"
    )
    for a, e in zip(sorted(actual, key=_sort_key), sorted(expected, key=_sort_key)):
        assert a.btype == e.btype == "tgbtc_mint"
        assert a.failed == e.failed, f"failed flag: {actual_tag}={a.failed} {expected_tag}={e.failed}"
        assert a.broken == e.broken, f"broken flag: {actual_tag}={a.broken} {expected_tag}={e.broken}"

        assert type(a.data) is type(e.data) is TgBTCMintData
        for f in fields(TgBTCMintData):
            av, ev = getattr(a.data, f.name), getattr(e.data, f.name)
            assert _values_equal(av, ev), (
                f"data.{f.name}: {actual_tag}={av!r} ({type(av).__name__}) "
                f"{expected_tag}={ev!r} ({type(ev).__name__})"
            )

        assert a.value_flow.to_dict() == e.value_flow.to_dict(), (
            f"value_flow: {actual_tag}={a.value_flow.to_dict()} "
            f"{expected_tag}={e.value_flow.to_dict()}"
        )
        assert _node_hashes(a) == _node_hashes(e), (
            f"consumed event nodes: {actual_tag}={_node_hashes(a)} "
            f"{expected_tag}={_node_hashes(e)}"
        )
        assert _topology_signature(a) == _topology_signature(e), (
            f"topology: {actual_tag}={_topology_signature(a)} "
            f"{expected_tag}={_topology_signature(e)}"
        )


async def _run_parity(trace) -> list[Block]:
    """legacy vs compiled, then compiled vs IR round trip. Returns the legacy
    blocks for per-case semantic pins."""
    hw_blocks = await _run_matcher(TgBTCMintBlockMatcher(), trace)

    compiled = compile_file(TGBTC_MCH, tgbtc_registries())
    by_name = {cm.name: cm.matcher for cm in compiled}
    dsl_blocks = await _run_matcher(by_name["tgbtc_mint"], trace)
    _assert_field_parity(dsl_blocks, hw_blocks, "dsl", "legacy")

    # IR emit + JSON render always run (break here must error, not skip).
    artifact = emit_ir(compiled, [TGBTC_MCH.name])
    render_json(artifact)
    if not _HAVE_IR:
        pytest.skip(_SKIP_REASON)

    art = _mch_ir.load_artifact(artifact)
    ir_by_name = {m.name: m for m in _mch_ir.build_matchers(art, tgbtc_registries())}
    ir_blocks = await _run_matcher(ir_by_name["tgbtc_mint"], trace)
    _assert_field_parity(ir_blocks, dsl_blocks, "ir", "compiled")
    return hw_blocks


@pytest.mark.asyncio
@pytest.mark.trace_file("iqIHDlbalqobuAMgJOC_T_9L6nLiwdOOr7BNKBtqIyA=.lz4")
async def test_tgbtc_mint_parity_short(trace_data):
    trace, _ = trace_data
    hw = await _run_parity(trace)
    assert hw[0].data.success
    assert hw[0].data.bitcoin_txid == "29473eb71be95904599a5cecd24f602f07942b1da2b4c5eb3c5551367c2e4023"


@pytest.mark.asyncio
@pytest.mark.trace_file("1OrxQbhicqml435pt0GPrXePkkdTGXRMlNJ4q67UZ-M=.lz4")
async def test_tgbtc_mint_parity_short_swapped_children(trace_data):
    trace, _ = trace_data
    hw = await _run_parity(trace)
    assert hw[0].data.success
    assert hw[0].data.bitcoin_txid == "c8254c7852b5e94c78f1667c29784a760e27c23c3cbf389705184944b4fa0716"


@pytest.mark.asyncio
@pytest.mark.trace_file("1rMRvEYh8TtCNox3P4VG6wcGLWX4YrU7jc55iqxzo6c=.lz4")
async def test_tgbtc_mint_parity_very_long_1(trace_data):
    trace, _ = trace_data
    hw = await _run_parity(trace)
    assert hw[0].data.success
    assert hw[0].data.bitcoin_txid == "e74d6db9342378bd2a5f09dac1aa7f4b035389c4ca92b8c112a500477009e0bf"


@pytest.mark.asyncio
@pytest.mark.trace_file("1qD3P7k62Wscr4J92DXfRmsFUoXsd631PVCjJDuZPi0=.lz4")
async def test_tgbtc_mint_parity_very_long_2(trace_data):
    trace, _ = trace_data
    hw = await _run_parity(trace)
    assert hw[0].data.success
    assert hw[0].data.bitcoin_txid == "b07fabef458e0b003895840ebba391d21e340fbb07b3ec95bc737f1b8d188219"
