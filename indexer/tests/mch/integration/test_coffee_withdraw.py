"""specs/coffee_withdraw.mch vs CoffeeWithdrawLiquidityMatcher.

This spec is the ONE port in the set without a row in test_parity_sweep.py, and
this file is why. The two matchers agree on every order-independent field and on
the consumed node set, but they disagree on WHICH payout is leg 1:

- the legacy matcher binds `payout_1`/`payout_2` by walking `block.next_blocks`
  in raw order, i.e. the order the trace's message rows happen to carry;
- the IR engine walks candidates in canonical order (ascending `min_lt`,
  spec §10.5) — the determinism property the whole engine is built on.

On the only corpus fixture those orders disagree, and the LEGACY one is wrong:
the pool's two payouts are stored 0004 before 0003, so legacy prints the event's
`amount1` (100000) next to the leg that actually moved `amount2` (100000000).
The canonical binding pairs each event amount with the payout that carries it.

So this file asserts three things:
  1. every field that does not depend on leg order is identical to legacy;
  2. the leg-order-dependent fields are exactly the legacy fields SWAPPED — no
     other divergence hides behind the swap;
  3. the IR pairing is the self-consistent one: `amount1_out` equals the amount
     actually moved by leg 1's jetton transfer, and likewise for leg 2.

(3) is the real regression guard. If the legacy matcher is ever fixed (order the
payout blocks by `min_lt` in its `build_block` and re-record the coffee.yaml
case), (2) starts failing and this file collapses into an ordinary parity test
plus a sweep row.

Fixture: b0QdK2TPX7Pv7NiXvpJxf_0PEumbW1FMewsnbFi-nqs= (coffee.yaml "Coffee
withdraw liquidity (jetton - jetton)"). No new recording.
"""
from __future__ import annotations

from pathlib import Path

import pytest

from indexer.events.blocks.basic_matchers import BlockMatcher
from indexer.events.blocks.core import Block
from indexer.events.blocks.jettons import (
    JettonBurnBlockMatcher,
    JettonTransferBlockMatcher,
)
from indexer.events.blocks.liquidity import CoffeeWithdrawLiquidityMatcher
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events import mch_ir
from indexer.events.mch import compile_file
from indexer.events.mch.builders.coffee import coffee_withdraw_registries
from indexer.events.mch.ir_emit import emit_ir
import indexer.events.mch.specs as specs_pkg

SPEC = Path(specs_pkg.__file__).parent / "coffee_withdraw.mch"
MATCHER_NAME = "coffee_withdraw_liquidity"

# The fields whose value depends on which payout the matcher called leg 1.
LEG_FIELDS = [
    ("asset1_out", "asset2_out"),
    ("dex_jetton_wallet_1", "dex_jetton_wallet_2"),
    ("wallet1", "wallet2"),
]
# amount1_out/amount2_out come from the withdrawal event, not from a payout, so
# they are the SAME on both sides — which is exactly what makes the leg swap
# observable rather than cosmetic.
SHARED_FIELDS = [
    "dex", "sender", "sender_wallet", "pool", "asset", "lp_tokens_burnt",
    "is_refund", "amount1_out", "amount2_out", "dex_wallet_1", "dex_wallet_2",
]


def _ir_matcher() -> BlockMatcher:
    compiled = compile_file(SPEC, coffee_withdraw_registries())
    art = mch_ir.load_artifact(emit_ir(compiled, [SPEC.name]))
    by_name = {m.name: m for m in mch_ir.build_matchers(art, coffee_withdraw_registries())}
    return by_name[MATCHER_NAME]


async def _run(matcher: BlockMatcher, trace, prereqs) -> list[Block]:
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    for pre in prereqs:
        for b in root.bfs_iter():
            if b.parent is None:
                await pre.try_build(b)
    produced: list[Block] = []
    for b in root.bfs_iter():
        if b.btype == "root" or b.parent is not None:
            continue
        r = await matcher.try_build(b)
        if r is not None:
            produced.extend(r)
    return [b for b in produced if b.btype == "dex_withdraw_liquidity"]


def _prereqs() -> list[BlockMatcher]:
    return [JettonTransferBlockMatcher(), JettonBurnBlockMatcher()]


def _data(block: Block) -> dict:
    d = block.data
    return dict(d) if isinstance(d, dict) else vars(d)


def _nodes(block: Block) -> list[str]:
    return sorted(n.message.tx_hash for n in block.event_nodes)


@pytest.mark.asyncio
@pytest.mark.trace_file("b0QdK2TPX7Pv7NiXvpJxf_0PEumbW1FMewsnbFi-nqs=.lz4")
async def test_coffee_withdraw_parity_and_leg_swap(trace_data):
    trace, _ = trace_data

    legacy = await _run(CoffeeWithdrawLiquidityMatcher(), trace, _prereqs())
    ir = await _run(_ir_matcher(), trace, _prereqs())

    assert len(legacy) == 1, f"fixture no longer yields one legacy block: {len(legacy)}"
    assert len(ir) == len(legacy)

    lb, ib = legacy[0], ir[0]
    assert ib.failed == lb.failed and ib.broken == lb.broken
    assert _nodes(ib) == _nodes(lb), "consumed node sets must be identical"

    ld, idt = _data(lb), _data(ib)
    assert set(ld) == set(idt)

    # (1) order-independent fields are identical.
    for key in SHARED_FIELDS:
        assert idt[key] == ld[key], f"data.{key}: ir={idt[key]!r} legacy={ld[key]!r}"

    # (2) the leg-dependent fields are legacy's, swapped — nothing else moved.
    for key1, key2 in LEG_FIELDS:
        assert idt[key1] == ld[key2], f"data.{key1}: ir={idt[key1]!r} legacy.{key2}={ld[key2]!r}"
        assert idt[key2] == ld[key1], f"data.{key2}: ir={idt[key2]!r} legacy.{key1}={ld[key1]!r}"


@pytest.mark.asyncio
@pytest.mark.trace_file("b0QdK2TPX7Pv7NiXvpJxf_0PEumbW1FMewsnbFi-nqs=.lz4")
async def test_coffee_withdraw_legs_are_self_consistent(trace_data):
    """(3) Each event amount sits next to the payout leg that actually moved it.

    The jetton transfer under each payout carries both the asset and the amount,
    so `amount1_out` must equal the amount of the transfer whose asset is
    `asset1_out`. This is the property the legacy binding violates.
    """
    trace, _ = trace_data
    ir = await _run(_ir_matcher(), trace, _prereqs())
    assert len(ir) == 1
    d = _data(ir[0])

    by_asset = {}
    node = to_tree(trace.transactions)
    root = Block("root", [])
    root.connect(init_block(node))
    for pre in _prereqs():
        for b in root.bfs_iter():
            if b.parent is None:
                await pre.try_build(b)
    for b in root.bfs_iter():
        if b.btype == "jetton_transfer":
            by_asset[str(b.data["asset"])] = b.data["amount"]

    assert by_asset, "fixture no longer carries the payout jetton transfers"
    assert by_asset[str(d["asset1_out"])] == d["amount1_out"], (
        f"leg 1 mismatch: amount1_out={d['amount1_out']!r} but the "
        f"{d['asset1_out']!r} transfer moved {by_asset[str(d['asset1_out'])]!r}"
    )
    assert by_asset[str(d["asset2_out"])] == d["amount2_out"], (
        f"leg 2 mismatch: amount2_out={d['amount2_out']!r} but the "
        f"{d['asset2_out']!r} transfer moved {by_asset[str(d['asset2_out'])]!r}"
    )
