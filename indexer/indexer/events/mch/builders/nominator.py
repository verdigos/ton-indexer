"""mch builder for the nominator_pool_withdraw matcher (specs/nominator.mch).

Remake (not a port) of blocks/staking.py NominatorPoolWithdrawMatcher — see the
spec file header for why the shape is re-anchored per payout instead of fanned
out from the NominatorPoolProcessWithdrawRequests call. Thin adapter over the
shared build core `build_nominator_pool_withdraw_core` in blocks/staking.py —
the same function the legacy matcher's thin wrapper calls per payout.
"""
from __future__ import annotations

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block
from indexer.events.blocks.messages.staking import NominatorPoolProcessWithdrawRequests
from indexer.events.blocks.staking import (
    NominatorPoolWithdrawRequestBlock,
    build_nominator_pool_withdraw_core,
)
from indexer.events.mch.registry import Registries

BLOCK_TYPES = {
    "NominatorPoolWithdrawRequestBlock": NominatorPoolWithdrawRequestBlock,
}


def register_nominator(reg: Registries) -> Registries:
    """Register everything specs/nominator.mch needs into `reg`."""
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_predicate("nominator_pool_withdraw_parent")
    def nominator_pool_withdraw_parent(block: Block) -> bool:
        parent = block.previous_block
        return (
            isinstance(parent, CallContractBlock)
            and parent.opcode == NominatorPoolProcessWithdrawRequests.opcode
        )

    @reg.register_builder("build_nominator_pool_withdraw")
    async def build_nominator_pool_withdraw(m):
        return await build_nominator_pool_withdraw_core(m.payout)

    return reg


def nominator_registries() -> Registries:
    """Fresh Registries ready to compile specs/nominator.mch."""
    return register_nominator(Registries())
