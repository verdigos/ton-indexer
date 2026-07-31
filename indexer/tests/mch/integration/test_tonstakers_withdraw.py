"""Field-level parity between the legacy tonstakers matchers and the three
matchers of specs/tonstakers.mch, PLUS the IR round trip.

Per matcher, two graphs are compared block-for-block at the T13-T17 bar (btype,
deep `data` equality, failed/broken flags, value_flow, consumed event-node set):

    legacy =  the hand-written matcher (blocks/staking.py)
    ir     =  compile_file -> emit_ir -> load_artifact -> build_matchers

tonstakers.mch is the first real end-to-end use of the produces-union (T11): the
withdraw build returns a TONStakersWithdrawBlock for the immediate arm and a
TONStakersWithdrawRequestBlock for the delayed arm. Both are exercised.

Prerequisite: NftMintBlockMatcher runs first (production registration order),
because the delayed arm reads its minted request NFT off
`delayed_withdrawal.next_blocks` — the matcher leaves that NftMint as its own
sibling block, so it is NOT a consumed pattern node (capturing it would diverge
the consumed set). The prereq is neutral tree preparation on both paths and a
no-op on the other fixtures.

Fixtures (tonstakers.yaml, no new recording) — all four corpus traces that carry
a TONStakers opcode, one per matcher arm:
- oND3Z5gMq1cKAe6TOJ5QAsqt-8_6wtG6X1hgkHP_Yhk=  immediate pool withdrawal
    (TONStakersWithdrawBlock, burnt_nft=None, asset set) — the
    `op TONStakersPoolWithdrawal` fork branch.
- GO-NI0EjOuhj1z4YzHK-VDAAOJsLm710DvyRRRMXhoY=  delayed withdrawal via minted
    request NFT (TONStakersWithdrawRequestBlock, minted_nft set) — the
    `op TONStakersMintNFT` fork branch, exercising the next_blocks NFT read.
- rbzKlyNgAhDnQ15r4tremaPVl_xURNgmSJQSyhGLSX4=  deposit.
- a0rjbO1XsPBt-2LXRQZT4piiD8x_OrfG82dU0in-WUc=  the delayed withdrawal's payout
    half (three independent NFT-burn chains in one trace).
"""
from __future__ import annotations

import dataclasses
from pathlib import Path

import pytest

from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.nft import NftMintBlockMatcher
from indexer.events.blocks.staking import (
    TONStakersDelayedWithdrawalMatcher,
    TONStakersDepositMatcher,
    TONStakersWithdrawMatcher,
)
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events import mch_ir
from indexer.events.mch import compile_file
from indexer.events.mch.builders.tonstakers import tonstakers_registries
from indexer.events.mch.ir_emit import emit_ir
import indexer.events.mch.specs as specs_pkg

TONSTAKERS_MCH = Path(specs_pkg.__file__).parent / "tonstakers.mch"
MATCHER_NAME = "tonstakers_withdraw"
TARGET_BTYPES = frozenset({"tonstakers_deposit", "tonstakers_withdraw",
                           "tonstakers_withdraw_request"})


def _prerequisites() -> list[BlockMatcher]:
    # The delayed arm reads its minted NFT off delayed_withdrawal.next_blocks;
    # NftMintBlockMatcher produces it and runs before TONStakersWithdrawMatcher.
    return [NftMintBlockMatcher()]


def _ir_matcher(name: str = MATCHER_NAME) -> BlockMatcher:
    # Compiling is a frontend-internal step ir_emit needs; the compiled matcher
    # is never run as a runtime (T29).
    compiled = compile_file(TONSTAKERS_MCH, tonstakers_registries())
    artifact = emit_ir(compiled, [TONSTAKERS_MCH.name])
    art = mch_ir.load_artifact(artifact)
    ir_by_name = {m.name: m for m in mch_ir.build_matchers(art, tonstakers_registries())}
    return ir_by_name[name]


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
    return [b for b in matches if b.btype in TARGET_BTYPES]


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
    assert len(reference) > 0, "reference produced no tonstakers_withdraw blocks - bad fixture"
    assert len(actual) == len(reference), (
        f"{label} count mismatch: actual={len(actual)} reference={len(reference)}"
    )
    for a, r in zip(sorted(actual, key=_sort_key), sorted(reference, key=_sort_key)):
        assert a.btype == r.btype, f"{label} btype: {a.btype} vs {r.btype}"
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


async def _run_two_way(trace, legacy=None, name: str = MATCHER_NAME) -> list[Block]:
    ir_matcher = _ir_matcher(name)
    legacy = legacy if legacy is not None else TONStakersWithdrawMatcher()
    hw_blocks = await _run_matcher(legacy, trace, _prerequisites())
    ir_blocks = await _run_matcher(ir_matcher, trace, _prerequisites())
    _assert_field_parity(ir_blocks, hw_blocks, "ir-vs-legacy")
    return hw_blocks


@pytest.mark.asyncio
@pytest.mark.trace_file("oND3Z5gMq1cKAe6TOJ5QAsqt-8_6wtG6X1hgkHP_Yhk=.lz4")
async def test_tonstakers_withdraw_immediate(trace_data):
    """Immediate pool withdrawal (produces-union -> TONStakersWithdrawBlock)."""
    trace, _ = trace_data
    hw_blocks = await _run_two_way(trace)
    assert hw_blocks[0].btype == "tonstakers_withdraw", (
        "fixture no longer exercises the immediate arm"
    )
    assert hw_blocks[0].data.burnt_nft is None


@pytest.mark.asyncio
@pytest.mark.trace_file("GO-NI0EjOuhj1z4YzHK-VDAAOJsLm710DvyRRRMXhoY=.lz4")
async def test_tonstakers_withdraw_delayed(trace_data):
    """Delayed withdrawal minting a request NFT (produces-union ->
    TONStakersWithdrawRequestBlock; minted NFT read from next_blocks)."""
    trace, _ = trace_data
    hw_blocks = await _run_two_way(trace)
    assert hw_blocks[0].btype == "tonstakers_withdraw_request", (
        "fixture no longer exercises the delayed arm"
    )
    assert hw_blocks[0].data.minted_nft is not None, (
        "fixture no longer has a minted request NFT"
    )


@pytest.mark.asyncio
@pytest.mark.trace_file("rbzKlyNgAhDnQ15r4tremaPVl_xURNgmSJQSyhGLSX4=.lz4")
async def test_tonstakers_deposit(trace_data):
    """Deposit: DepositRequest -> MintJettons -> InternalTransfer (-> Notify).

    Pins the DEAD-BRANCH decision documented in the spec: legacy declares the
    MintJettons node optional, but its build core dereferences the transfer with
    no null guard, so `transfer is None` raises and try_build swallows it into a
    clean rejection. The spec makes the node mandatory instead, which is why this
    parity check can hold at all — the two engines would otherwise disagree on
    any trace with a deposit request and no mint leg.
    """
    trace, _ = trace_data
    hw_blocks = await _run_two_way(trace, TONStakersDepositMatcher(), "tonstakers_deposit")
    assert len(hw_blocks) == 1
    assert hw_blocks[0].btype == "tonstakers_deposit"
    assert hw_blocks[0].data.tokens_minted is not None, (
        "fixture no longer exercises the minted (non-failed) shape"
    )


@pytest.mark.asyncio
@pytest.mark.trace_file("a0rjbO1XsPBt-2LXRQZT4piiD8x_OrfG82dU0in-WUc=.lz4")
async def test_tonstakers_delayed_withdrawal(trace_data):
    """The second half of a delayed withdrawal: the request NFT is burned and
    the pool pays out (DistributedAsset <- NftBurnNotification <- NftBurn).

    The fixture carries THREE independent burn chains, so it also pins that the
    parent-chain anchor fires once per payout. `pool` comes from the
    `tonstakers_pool_addr` host fn walking previous_block up to the 0x1140a64f
    start-asset-distribution call, which this fixture does contain.
    """
    trace, _ = trace_data
    hw_blocks = await _run_two_way(
        trace, TONStakersDelayedWithdrawalMatcher(), "tonstakers_delayed_withdrawal"
    )
    assert len(hw_blocks) == 3, "fixture no longer carries three burn chains"
    for b in hw_blocks:
        assert b.btype == "tonstakers_withdraw"
        assert b.data.burnt_nft is not None
        assert b.data.asset is None
        assert b.data.pool is not None, "the previous_block pool walk found nothing"
