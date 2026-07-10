"""R07 remake — three-way parity (legacy == compiled == v2 IR) between the
GetgemsNftPurchaseBlockMatcher and specs/getgems.mch, plus the shaper's proxy
surgery on a synthetic finish/stop tree.

Assertion depth matches test_telegram_nft / test_dedust_deposit: block class +
btype, per-key `data` equality, failed/broken flags, value_flow, consumed
event-node set, AND block topology (children_blocks / next_blocks multisets) —
the M2R remake gate binds on topology because the action hierarchy derives from
it, and here the shaper mutates topology by inserting an EmptyBlock proxy.

Getgems anchors on the already-produced `nft_transfer` block, so both the legacy
matcher and the mch matcher run AFTER NftTransferBlockMatcher (the production
registration order). The prereq runs on each independently built tree.

Fixtures (nft.yaml, real traces): "Getgems purchase" (direct sale, two payout
children), "Getgems auction purchase" (single Profit payout), "Getgems auction
purchase 2" (Profit + Marketplace fee children). All three are the non-proxy
shape — the seller payout is a direct child of the nft_transfer.

Corpus gap (no recording per task rules): no finish/stop sale-contract trace
where the payout is a SIBLING and the EmptyBlock proxy is inserted. That branch
is unlike R01's consumed parent — legacy leaves the finish parent UNCONSUMED
while consuming the sibling payout, a shape the mch tree grammar cannot capture
without consuming the parent (a consumed-set / topology parity break). It stays
legacy-only for matching; the lifted proxy surgery (the getgems_proxy_insert
shaper) is proven byte-for-byte against the legacy surgery on a synthetic tree
below.
"""
from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

import pytest

from indexer.events import context
from indexer.events.blocks.basic_blocks import TonTransferBlock
from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block, EmptyBlock
from indexer.events.blocks.nft import (
    GetgemsNftPurchaseBlockMatcher,
    NftPurchaseBlock,
    NftTransferBlockMatcher,
)
from indexer.events.blocks.utils import AccountId, Amount
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events import mch_ir
from indexer.events.mch import compile_file
from indexer.events.mch.builders.getgems import getgems_registries
from indexer.events.mch.ir_emit import emit_ir, render_json
import indexer.events.mch.specs as specs_pkg
from tests.utils.repository import TestInterfaceRepository
from tests.utils.trace_deserializer import load_trace_from_file

GETGEMS_MCH = Path(specs_pkg.__file__).parent / "getgems.mch"
MATCHER_NAME = "getgems_nft_purchase"

# (fixture, id) — the nft.yaml getgems corpus.
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


async def _run_side(matcher: BlockMatcher, trace, prereqs=()) -> list[Block]:
    """One tree per side; prerequisites first, then the matcher over a full BFS
    pass (mirrors process_event_async)."""
    root = _build_block_tree(trace)
    for pre in prereqs:
        for b in root.bfs_iter():
            if b.parent is None:
                await pre.try_build(b)
    produced: list[Block] = []
    for b in root.bfs_iter():
        if b.btype == "root" or b.parent is not None:
            continue
        result = await matcher.try_build(b)
        if result is not None:
            produced.extend(result)
    # Wave-B: the hybrid build produces a plain Block with btype 'nft_purchase'
    # (not a NftPurchaseBlock instance); filter on btype, not class identity.
    return [b for b in produced if b.btype == "nft_purchase"]


def _values_equal(a, b) -> bool:
    if a is None and b is None:
        return True
    if type(a) is not type(b):
        return False
    return bool(a == b)


def _data_keys(block: Block) -> set:
    """Field names of the produced block's data, dict (IR hybrid) or NftPurchaseData
    dataclass (legacy) alike."""
    d = block.data
    if isinstance(d, dict):
        return set(d.keys())
    return {f.name for f in d.__dataclass_fields__.values()}


def _data_get(block: Block, key: str):
    d = block.data
    return d[key] if isinstance(d, dict) else getattr(d, key)


def _node_hashes(block: Block) -> list[str]:
    return sorted(n.message.tx_hash for n in block.event_nodes)


def _sort_key(block: Block):
    return (block.min_lt, tuple(_node_hashes(block)))


def _topology(block: Block) -> tuple[list, list]:
    """(children_blocks, next_blocks) as (btype, sorted-node-hashes) multisets.
    An EmptyBlock proxy has no event nodes, so it shows up as ('empty', ())."""
    def sig(bs):
        return sorted((b.btype, tuple(_node_hashes(b))) for b in bs)
    return sig(block.children_blocks), sig(block.next_blocks)


def _assert_parity(actual: list[Block], expected: list[Block],
                   actual_tag: str, expected_tag: str) -> None:
    assert len(expected) > 0, f"{expected_tag} produced no nft_purchase blocks - bad fixture"
    assert len(actual) == len(expected), (
        f"count mismatch: {actual_tag}={len(actual)} {expected_tag}={len(expected)}"
    )
    for a, e in zip(sorted(actual, key=_sort_key), sorted(expected, key=_sort_key)):
        # Wave-B: IR hybrid produces a plain Block(dict); legacy a NftPurchaseBlock
        # (dataclass). Assert btype + per-field data, not class/dataclass identity.
        assert a.btype == e.btype == "nft_purchase"
        assert a.failed == e.failed, f"failed flag: {actual_tag}={a.failed} {expected_tag}={e.failed}"
        assert a.broken == e.broken, f"broken flag: {actual_tag}={a.broken} {expected_tag}={e.broken}"

        akeys, ekeys = _data_keys(a), _data_keys(e)
        assert akeys == ekeys, f"data fields: {actual_tag}={sorted(akeys)} {expected_tag}={sorted(ekeys)}"
        for key in sorted(ekeys):
            av, ev = _data_get(a, key), _data_get(e, key)
            assert _values_equal(av, ev), (
                f"data.{key}: {actual_tag}={av!r} ({type(av).__name__}) "
                f"{expected_tag}={ev!r} ({type(ev).__name__})"
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
    # Compiling is a frontend-internal step ir_emit needs; no compiled matcher is
    # run as a runtime (T29).
    compiled = compile_file(GETGEMS_MCH, getgems_registries())
    artifact = emit_ir(compiled, [GETGEMS_MCH.name])
    render_json(artifact)  # emit path must not raise
    art = mch_ir.load_artifact(artifact)
    ir_by_name = {m.name: m for m in mch_ir.build_matchers(art, getgems_registries())}
    return ir_by_name[MATCHER_NAME]


@pytest.mark.asyncio
@pytest.mark.parametrize("filename", [c[0] for c in CASES], ids=[c[1] for c in CASES])
async def test_getgems_purchase_parity(traces_dir, filename):
    trace = _load_trace(traces_dir, filename)
    ir_matcher = _ir_matcher()

    hw = await _run_side(GetgemsNftPurchaseBlockMatcher(), trace, prereqs=(NftTransferBlockMatcher(),))
    ir = await _run_side(ir_matcher, trace, prereqs=(NftTransferBlockMatcher(),))

    assert len(hw) > 0, "legacy matcher produced no nft_purchase blocks - stale fixture"
    _assert_parity(ir, hw, "ir", "legacy")


# ---------------------------------------------------------------------------
# Synthetic finish/stop proxy tree (no corpus coverage per task rules): the
# seller payout is a SIBLING under a finish/stop parent, and the produced block
# gets an EmptyBlock proxy inserted between that (unconsumed) parent and
# [nft_transfer, payout]. Exercises the getgems_proxy_insert shaper through the
# compiled AND IR engines, asserting three-way topology parity incl. the proxy.
# Blocks are hand-built with fake event nodes (test_shape.py convention).
# ---------------------------------------------------------------------------
_SELLER = "0:" + "aa" * 32
_BUYER = "0:" + "bb" * 32
_SALE = "0:" + "cc" * 32
_NFT = "0:" + "dd" * 32
_MP = "0:" + "ee" * 32
_COLL = "0:" + "11" * 32


class _FakeTx:
    def __init__(self, lt: int):
        self.lt = lt
        self.now = lt
        self.orig_status = "active"
        self.end_status = "active"
        self.total_fees = 0
        self.emulated = False
        self.messages = []


class _FakeMsg:
    def __init__(self, tx_hash: str, lt: int, src: str, dst: str, value: int):
        self.tx_hash = tx_hash
        self.created_lt = lt
        self.created_at = lt
        self.source = src
        self.destination = dst
        self.value = value
        self.direction = "in"
        self.opcode = None
        self.bounce = False
        self.value_extra_currencies = None
        self.fwd_fee = None
        self.transaction = _FakeTx(lt)
        self.transaction.messages = [self]
        self.message_content = SimpleNamespace(body=None)


class _FakeNode:
    is_tick_tock = False
    tick_tock_tx = None
    ghost_node = False
    parent = None

    def __init__(self, msg: _FakeMsg):
        self.message = msg
        self.failed = False

    def get_lt(self):
        return self.message.created_lt

    def get_utime(self):
        return self.message.created_at

    def get_opcode(self):
        return 0

    def get_tx(self):
        return self.message.transaction


def _mk_tt(tx_hash, lt, src, dst, value, comment=None) -> TonTransferBlock:
    b = Block.__new__(TonTransferBlock)
    node = _FakeNode(_FakeMsg(tx_hash, lt, src, dst, value))
    Block.__init__(b, "ton_transfer", [node], {
        "source": AccountId(src), "destination": AccountId(dst),
        "value": Amount(value), "comment": comment,
    })
    b.value = value
    b.comment = comment
    b.encrypted = False
    b.comment_encoded = False
    return b


def _mk_getgems_nft(tx_hash, lt) -> Block:
    return Block("nft_transfer", [_FakeNode(_FakeMsg(tx_hash, lt, _SALE, _NFT, 0))], {
        "is_purchase": True,
        "marketplace": "getgems",
        "marketplace_address": AccountId(_MP),
        "real_prev_owner": AccountId(_SELLER),
        "price": Amount(1000),
        "nft": {"address": AccountId(_NFT), "index": 5,
                "collection": {"address": AccountId(_COLL)}, "exists": True},
        "prev_owner": AccountId(_SELLER),
        "new_owner": AccountId(_BUYER),
        "query_id": 42,
        "forward_amount": Amount(1),
        "response_destination": AccountId(_BUYER),
        "custom_payload": None,
        "forward_payload": None,
    })


def _build_proxy_tree(comment: str) -> Block:
    """finish/stop parent -> [nft_transfer(getgems), payout(to seller)]."""
    root = Block("root", [])
    parent = _mk_tt("FINISH", 10, _BUYER, _SALE, 5000, comment=comment)
    nft = _mk_getgems_nft("NFT", 20)
    payout = _mk_tt("PAYOUT", 30, _SALE, _SELLER, 900)
    root.connect(parent)
    parent.connect(nft)
    parent.connect(payout)
    return root


async def _run_side_tree(matcher: BlockMatcher, root: Block) -> list[Block]:
    produced: list[Block] = []
    for b in root.bfs_iter():
        if b.btype == "root" or b.parent is not None:
            continue
        result = await matcher.try_build(b)
        if result is not None:
            produced.extend(result)
    return [b for b in produced if b.btype == "nft_purchase"]


@pytest.mark.asyncio
@pytest.mark.parametrize("comment", ["finish", "stop"])
async def test_getgems_finish_stop_proxy_parity(comment):
    ir_matcher = _ir_matcher()

    hw = await _run_side_tree(GetgemsNftPurchaseBlockMatcher(), _build_proxy_tree(comment))
    ir = await _run_side_tree(ir_matcher, _build_proxy_tree(comment))

    assert len(hw) == 1, "legacy should build exactly one proxy nft_purchase"
    # The proxy branch actually inserted an EmptyBlock (topology carries it).
    assert any(isinstance(c, EmptyBlock) for c in hw[0].children_blocks), "no proxy inserted"

    _assert_parity(ir, hw, "ir", "legacy")
