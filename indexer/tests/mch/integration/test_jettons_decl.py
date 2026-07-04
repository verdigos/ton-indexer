"""Field-level parity between the legacy jetton matchers and the FULLY
DECLARATIVE spec (specs_decl/jettons.mch) — build statements + `out {...}`, no
registered builder.

Same fixtures and acceptance bar as test_jettons_family.py: for each fixture the
legacy matcher and the declarative-compiled matcher run on independently built
block trees, and every produced block is compared field by field — btype,
`data` dict (deep, per-key), `failed`/`broken` flags, value_flow, and the
consumed event-node set (sorted tx hashes). The declarative path produces a
generic Block, so parity is on `.btype` (normalized string), not the class.
"""
from __future__ import annotations

from pathlib import Path

import pytest

import indexer.events.mch as mch
from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.jettons import (
    JettonBurnBlockMatcher,
    JettonMintBlockMatcher,
    JettonTransferBlockMatcher,
)
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_file
from indexer.events.mch.builders import jetton_decl_registries

JETTONS_DECL_MCH = Path(mch.__file__).parent / "specs_decl" / "jettons.mch"

LEGACY_MATCHERS = {
    "jetton_transfer": JettonTransferBlockMatcher,
    "jetton_burn": JettonBurnBlockMatcher,
    "jetton_mint": JettonMintBlockMatcher,
}


def _compile_decl_matcher(name: str) -> BlockMatcher:
    compiled = compile_file(JETTONS_DECL_MCH, jetton_decl_registries())
    by_name = {cm.name: cm.matcher for cm in compiled}
    return by_name[name]


def _build_block_tree(trace) -> Block:
    """Replicate event_processing's prelude: tree of EventNodes -> tree of Blocks."""
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    return root


async def _run_matcher(matcher: BlockMatcher, trace) -> list:
    """Run a matcher across a fresh block tree, mirroring process_event_async."""
    root = _build_block_tree(trace)
    matches: list = []
    for b in root.bfs_iter():
        if b.btype == "root":
            continue
        if b.parent is not None:
            continue
        result = await matcher.try_build(b)
        if result is not None:
            matches.extend(result)
    return matches


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


def _assert_field_parity(decl_blocks: list[Block], hw_blocks: list[Block], btype: str) -> None:
    assert len(hw_blocks) > 0, f"legacy matcher produced no {btype} blocks - bad fixture"
    assert len(decl_blocks) == len(hw_blocks), (
        f"count mismatch: decl={len(decl_blocks)} legacy={len(hw_blocks)}"
    )
    for d, h in zip(sorted(decl_blocks, key=_sort_key), sorted(hw_blocks, key=_sort_key)):
        assert d.btype == h.btype == btype
        assert d.failed == h.failed, f"failed flag: decl={d.failed} legacy={h.failed}"
        assert d.broken == h.broken, f"broken flag: decl={d.broken} legacy={h.broken}"

        assert set(d.data.keys()) == set(h.data.keys()), (
            f"data keys: decl={sorted(d.data)} legacy={sorted(h.data)}"
        )
        for key in sorted(h.data):
            dv, hv = d.data[key], h.data[key]
            assert _values_equal(dv, hv), (
                f"data[{key!r}]: decl={dv!r} ({type(dv).__name__}) "
                f"legacy={hv!r} ({type(hv).__name__})"
            )

        assert d.value_flow.to_dict() == h.value_flow.to_dict(), (
            f"value_flow: decl={d.value_flow.to_dict()} legacy={h.value_flow.to_dict()}"
        )
        assert _node_hashes(d) == _node_hashes(h), (
            f"consumed event nodes: decl={_node_hashes(d)} legacy={_node_hashes(h)}"
        )


async def _run_parity(trace, btype: str, anchor_opcode: int | None = None) -> None:
    hw_results = await _run_matcher(LEGACY_MATCHERS[btype](), trace)
    decl_results = await _run_matcher(_compile_decl_matcher(btype), trace)

    hw_blocks = [b for b in hw_results if b.btype == btype]
    decl_blocks = [b for b in decl_results if b.btype == btype]
    _assert_field_parity(decl_blocks, hw_blocks, btype)

    if anchor_opcode is not None:
        assert any(
            getattr(cb, "opcode", None) == anchor_opcode
            for b in hw_blocks
            for cb in b.children_blocks
        ), f"fixture does not exercise anchor opcode {anchor_opcode:#x}"


@pytest.mark.asyncio
@pytest.mark.trace_file("KYQfjYD1YybzQIuoJwkoXL-Y-apztmw6AGjIcPhotjQ=.lz4")
async def test_jetton_transfer_parity(trace_data):
    trace, _ = trace_data
    await _run_parity(trace, "jetton_transfer")


@pytest.mark.asyncio
@pytest.mark.trace_file("xBl_8BTUqSv52IWAzrmg54-V0YR_WQsV1buJxKBq4vE=.lz4")
async def test_jetton_burn_parity(trace_data):
    trace, _ = trace_data
    await _run_parity(trace, "jetton_burn")


@pytest.mark.asyncio
@pytest.mark.trace_file("b0QdK2TPX7Pv7NiXvpJxf_0PEumbW1FMewsnbFi-nqs=.lz4")
async def test_jetton_burn_parity_lp_burn(trace_data):
    trace, _ = trace_data
    await _run_parity(trace, "jetton_burn")


@pytest.mark.asyncio
@pytest.mark.trace_file("SVotcOQBV7MZ-CdW7WsaOucRods9-SF0vnl646rxsBc=.lz4")
async def test_jetton_mint_parity_minter_opcode(trace_data):
    """MinterJettonMint (0x15) branch of the opcode-set anchor."""
    trace, _ = trace_data
    await _run_parity(trace, "jetton_mint", anchor_opcode=0x00000015)


@pytest.mark.asyncio
@pytest.mark.trace_file("uXA5LQpf_4c9NEOK6trJZuBMitcBLNYxK7_2OfzphtI=.lz4")
async def test_jetton_mint_parity_mint_opcode(trace_data):
    """JettonMint (0x642b7d07) branch of the opcode-set anchor, with notification."""
    trace, _ = trace_data
    await _run_parity(trace, "jetton_mint", anchor_opcode=0x642b7d07)


@pytest.mark.asyncio
@pytest.mark.trace_file("MyrAUqqz_XH2D5bUdRd-YYCVo7JEEzjWX00gGXRFLLY=.lz4")
async def test_jetton_mint_parity_failed(trace_data):
    """Failed mint - exercises the failed-flag / fallback-data path."""
    trace, _ = trace_data
    await _run_parity(trace, "jetton_mint", anchor_opcode=0x642b7d07)
