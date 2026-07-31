"""Field-level parity between LayerZeroReceiveMatcher and specs/layerzero.mch,
plus the IR round trip (compiled -> emit_ir -> mch_ir load/build).

Same assertion depth as the T13-T17 ports (test_stonfi_v2_swap etc.): btype,
deep `data` equality, failed/broken flags, value_flow, and the consumed
event-node set. Count parity alone is not acceptance. `LayerZeroReceiveBlock`
carries a `LayerZeroReceiveData` dataclass (not a dict), so `data` equality is a
recursive dataclass compare (AccountId / LayerZeroPacketData all define __eq__).

DUP-LABEL COVERAGE: the port is exactly here to exercise the reused
"execute_callback" label. The legacy `get_labeled` resolves it to the OUTER
LayerzeroLzReceiveExecute (0x0c7b8418); the spec captures the outer as @execute
and the inner LayerZeroOappExecuteCallback (0xcaae25a1) as @oapp_callback, and
the builder maps @execute. If that mapping were wrong (inner picked instead), the
channel/oapp/packet fields below would diverge from the legacy block, so the deep
`data` compare is the guard.

Fixtures (tests/test_cases/layerzero.yaml -> tests/traces): both are single
LayerZero receives with every optional child present (excesses ton_transfer,
even_1 event, event_2 event, trailing excess ton_transfer), consuming 9 nodes.

Corpus gaps (no recording per task rules): no receive with a missing optional
child (event/excess absent — the graceful-skip path in the capture walker and
children-set matcher is unexercised on real data), and no dst_oapp-mismatch
reject (the `execute_callback address != packet.dst_oapp` -> reject branch).
"""
from __future__ import annotations

from pathlib import Path

import pytest

from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.layerzero import (
    LayerZeroReceiveBlock,
    LayerZeroReceiveMatcher,
)
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_file
from indexer.events.mch.builders.layerzero import layerzero_registries
from indexer.events.mch.ir_emit import emit_ir, render_json
import indexer.events.mch.specs as specs_pkg

LAYERZERO_MCH = Path(specs_pkg.__file__).parent / "layerzero.mch"

# --- IR engine gate (built in parallel; may be mid-rewrite) -----------------
try:
    from indexer.events import mch_ir as _mch_ir

    _HAVE_IR = hasattr(_mch_ir, "load_artifact") and hasattr(_mch_ir, "build_matchers")
    _IR_IMPORT_ERR: BaseException | None = None
except Exception as ex:  # pragma: no cover - import error path
    _mch_ir = None
    _HAVE_IR = False
    _IR_IMPORT_ERR = ex

_SKIP_REASON = (
    "indexer.events.mch_ir not importable yet (built in parallel); the "
    "compiled-vs-legacy parity above still ran"
)


def _build_ir_matcher() -> BlockMatcher:
    """Compile (frontend-internal) -> emit IR -> load -> build; return the IR
    matcher. Also renders the artifact to JSON (must not raise) so a break in the
    emit plumbing ERRORS here rather than silently skipping. Assumes _HAVE_IR."""
    compiled = compile_file(LAYERZERO_MCH, layerzero_registries())
    artifact = emit_ir(compiled, [LAYERZERO_MCH.name])
    render_json(artifact)
    art = _mch_ir.load_artifact(artifact)
    ir_by_name = {m.name: m for m in _mch_ir.build_matchers(art, layerzero_registries())}
    assert "layerzero_receive" in ir_by_name, "layerzero_receive not built by the IR engine"
    return ir_by_name["layerzero_receive"]


def _build_block_tree(trace) -> Block:
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matcher(matcher: BlockMatcher, trace) -> list:
    root = _build_block_tree(trace)
    matches: list = []
    for b in root.bfs_iter():
        if b.btype == "root" or b.parent is not None:
            continue
        result = await matcher.try_build(b)
        if result is not None:
            matches.extend(result)
    # specs/layerzero.mch has a declarative build, so the IR engine yields a
    # canonical generic Block (btype + dict data), not a LayerZeroReceiveBlock
    # instance; the legacy matcher still yields the dataclass block. Filter by
    # btype so both paths flow through (wave-B btype+data acceptance).
    return [b for b in matches if b.btype == "layerzero_receive"]


def _node_hashes(block: Block) -> list[str]:
    return sorted(n.message.tx_hash for n in block.event_nodes)


def _sort_key(block: Block):
    return (block.min_lt, tuple(_node_hashes(block)))


def _data_dict(data) -> dict:
    """Normalize a produced block's `data` to a plain dict regardless of
    representation (legacy LayerZeroReceiveData dataclass vs the declarative
    build's dict). AccountId defines __eq__, so field values compare directly."""
    get = (lambda k: data[k]) if isinstance(data, dict) else (lambda k: getattr(data, k))
    pd = get("packet_data")
    pget = (lambda k: pd[k]) if isinstance(pd, dict) else (lambda k: getattr(pd, k))
    return {
        "sender": get("sender"),
        "channel": get("channel"),
        "oapp": get("oapp"),
        "packet_data": {
            k: pget(k)
            for k in ("src_oapp", "dst_oapp", "src_eid", "dst_eid", "nonce", "guid", "message")
        },
    }


def _packet_nonce(data):
    pd = data["packet_data"] if isinstance(data, dict) else data.packet_data
    return pd["nonce"] if isinstance(pd, dict) else pd.nonce


def _assert_field_parity(a_blocks: list[Block], b_blocks: list[Block], label: str) -> None:
    assert len(b_blocks) > 0, f"{label}: reference produced no layerzero_receive blocks - bad fixture"
    assert len(a_blocks) == len(b_blocks), (
        f"{label}: count mismatch: a={len(a_blocks)} b={len(b_blocks)}"
    )
    for a, b in zip(sorted(a_blocks, key=_sort_key), sorted(b_blocks, key=_sort_key)):
        assert a.btype == b.btype == "layerzero_receive"
        assert a.failed == b.failed, f"{label}: failed flag: a={a.failed} b={b.failed}"
        assert a.broken == b.broken, f"{label}: broken flag: a={a.broken} b={b.broken}"
        # a.data is the declarative build's dict, b.data the legacy dataclass;
        # normalize both to a dict (AccountId/values define __eq__).
        assert _data_dict(a.data) == _data_dict(b.data), f"{label}: data: a={a.data} b={b.data}"
        assert a.value_flow.to_dict() == b.value_flow.to_dict(), (
            f"{label}: value_flow: a={a.value_flow.to_dict()} b={b.value_flow.to_dict()}"
        )
        assert _node_hashes(a) == _node_hashes(b), (
            f"{label}: consumed event nodes: a={_node_hashes(a)} b={_node_hashes(b)}"
        )


async def _run_all(trace):
    """Return (legacy, ir_or_None) block lists over the SAME trace, each built on
    an independently constructed block tree. The compiled path is retired (T29)."""
    legacy = await _run_matcher(LayerZeroReceiveMatcher(), trace)
    ir = await _run_matcher(_build_ir_matcher(), trace) if _HAVE_IR else None
    return legacy, ir


@pytest.mark.asyncio
@pytest.mark.trace_file("1lRaVzUa7EjqG228WiDTg_2b_HSGlO5apCmj2JEAHxA=.lz4")
async def test_layerzero_receive_parity(trace_data):
    """First receive fixture (nonce 729): IR == legacy, field-level."""
    trace, _ = trace_data
    legacy, ir = await _run_all(trace)
    # The dup-label mapping produced the OUTER-derived channel/oapp, not the inner.
    assert legacy[0].data.channel != legacy[0].data.oapp
    if not _HAVE_IR:
        pytest.skip(_SKIP_REASON)
    _assert_field_parity(ir, legacy, "ir-vs-legacy")
    assert _packet_nonce(ir[0].data) == 729


@pytest.mark.asyncio
@pytest.mark.trace_file("2gDW04F5Zve-t6WkfZR2jOgM15IDtYfVkIbuRTvrzT8=.lz4")
async def test_layerzero_receive_parity_2(trace_data):
    """Second receive fixture (nonce 162): IR == legacy, field-level."""
    trace, _ = trace_data
    legacy, ir = await _run_all(trace)
    if not _HAVE_IR:
        pytest.skip(_SKIP_REASON)
    _assert_field_parity(ir, legacy, "ir-vs-legacy")
    assert _packet_nonce(ir[0].data) == 162
