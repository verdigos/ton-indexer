"""T13 — field-level parity between legacy jetton matchers and specs/jettons.mch.

For each fixture the legacy matcher and the mch-compiled matcher run on two
independently built block trees; produced blocks are then compared field by
field: btype, `data` dict (deep, per-key), `failed`/`broken` flags, value_flow,
and the consumed event-node set (sorted tx hashes). Count parity alone is not
acceptance.

Fixtures (trace-id -> YAML case):
- jetton_transfer: KYQfjYD1YybzQIuoJwkoXL-Y-apztmw6AGjIcPhotjQ=
    dedust-liquidities.yaml (two successful transfers with internal+notify legs)
- jetton_burn: xBl_8BTUqSv52IWAzrmg54-V0YR_WQsV1buJxKBq4vE=
    tgbtc.yaml "tgBTC burn" (plain JettonBurn + notification)
- jetton_burn: b0QdK2TPX7Pv7NiXvpJxf_0PEumbW1FMewsnbFi-nqs=
    coffee.yaml "Coffee withdraw liquidity (jetton - jetton)" (LP-token burn)
- jetton_mint: SVotcOQBV7MZ-CdW7WsaOucRods9-SF0vnl646rxsBc=
    jetton-mints.yaml "Jetton mint(0x15) 1" (MinterJettonMint anchor opcode)
- jetton_mint: uXA5LQpf_4c9NEOK6trJZuBMitcBLNYxK7_2OfzphtI=
    jetton-mints.yaml "Jetton mint(0x642b7d07) with notification" (JettonMint anchor)
- jetton_mint: MyrAUqqz_XH2D5bUdRd-YYCVo7JEEzjWX00gGXRFLLY=
    jetton-mints.yaml "Jetton mint(0x642b7d07) failed" (failed-path build)
"""
from __future__ import annotations

from pathlib import Path

import pytest

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
from indexer.events.mch.builders import jetton_registries
import indexer.events.mch.specs as specs_pkg

JETTONS_MCH = Path(specs_pkg.__file__).parent / "jettons.mch"

LEGACY_MATCHERS = {
    "jetton_transfer": JettonTransferBlockMatcher,
    "jetton_burn": JettonBurnBlockMatcher,
    "jetton_mint": JettonMintBlockMatcher,
}


def _compile_dsl_matcher(name: str) -> BlockMatcher:
    compiled = compile_file(JETTONS_MCH, jetton_registries())
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


def _assert_field_parity(dsl_blocks: list[Block], hw_blocks: list[Block], btype: str) -> None:
    assert len(hw_blocks) > 0, f"legacy matcher produced no {btype} blocks - bad fixture"
    assert len(dsl_blocks) == len(hw_blocks), (
        f"count mismatch: dsl={len(dsl_blocks)} legacy={len(hw_blocks)}"
    )
    for d, h in zip(sorted(dsl_blocks, key=_sort_key), sorted(hw_blocks, key=_sort_key)):
        assert d.btype == h.btype == btype
        assert d.failed == h.failed, f"failed flag: dsl={d.failed} legacy={h.failed}"
        assert d.broken == h.broken, f"broken flag: dsl={d.broken} legacy={h.broken}"

        assert set(d.data.keys()) == set(h.data.keys()), (
            f"data keys: dsl={sorted(d.data)} legacy={sorted(h.data)}"
        )
        for key in sorted(h.data):
            dv, hv = d.data[key], h.data[key]
            assert _values_equal(dv, hv), (
                f"data[{key!r}]: dsl={dv!r} ({type(dv).__name__}) "
                f"legacy={hv!r} ({type(hv).__name__})"
            )

        assert d.value_flow.to_dict() == h.value_flow.to_dict(), (
            f"value_flow: dsl={d.value_flow.to_dict()} legacy={h.value_flow.to_dict()}"
        )
        assert _node_hashes(d) == _node_hashes(h), (
            f"consumed event nodes: dsl={_node_hashes(d)} legacy={_node_hashes(h)}"
        )


async def _run_parity(trace, btype: str, anchor_opcode: int | None = None) -> None:
    hw_results = await _run_matcher(LEGACY_MATCHERS[btype](), trace)
    dsl_results = await _run_matcher(_compile_dsl_matcher(btype), trace)

    hw_blocks = [b for b in hw_results if b.btype == btype]
    dsl_blocks = [b for b in dsl_results if b.btype == btype]
    _assert_field_parity(dsl_blocks, hw_blocks, btype)

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
    """Failed mint - exercises the failed-flag path of the shared build core."""
    trace, _ = trace_data
    await _run_parity(trace, "jetton_mint", anchor_opcode=0x642b7d07)
