"""mch builders for the dedust matchers (specs/dedust_swap.mch,
specs/dedust_withdraw.mch).

Swap: the legacy build (blocks/swaps.py build_dedust_swap_core) scans the full
consumed block set — find_messages/find_call_contracts over other_blocks plus
`in include` membership checks — which captures cannot reconstruct (blocks
matched by the recursion's exit branch have no bindings). The builder therefore
uses the well-known `Match._mch_consumed` attribute (anchor first) and
delegates to the shared core. Returns None to reject the match (spec §10.7).

Withdraw: no recursion in the pattern, all captures bind positionally — the
builder is fully capture-driven (anchor + the two payout terminals).
"""
from __future__ import annotations

from indexer.events.blocks.core import Block
from indexer.events.blocks.liquidity import build_dedust_withdraw_core
from indexer.events.blocks.messages import (
    DedustPayout,
    DedustPayoutFromPool,
    DedustSwap,
    DedustSwapExternal,
    DedustSwapNotification,
    DedustSwapPeer,
)
from indexer.events.blocks.messages.jettons import (
    JettonBurn,
    JettonBurnNotification,
)
from indexer.events.blocks.swaps import JettonSwapBlock, build_dedust_swap_core
from indexer.events.mch.registry import Registries

OPCODES = {
    "DedustSwapExternal":     DedustSwapExternal.opcode,
    "DedustSwapPeer":         DedustSwapPeer.opcode,
    "DedustSwap":             DedustSwap.opcode,
    "DedustSwapNotification": DedustSwapNotification.opcode,
    "DedustPayoutFromPool":   DedustPayoutFromPool.opcode,
    "DedustPayout":           DedustPayout.opcode,
}

BLOCK_TYPES = {
    "JettonSwapBlock": JettonSwapBlock,
}

WITHDRAW_OPCODES = {
    "JettonBurn":             JettonBurn.opcode,
    "JettonBurnNotification": JettonBurnNotification.opcode,
    "DedustPayoutFromPool":   DedustPayoutFromPool.opcode,
    "DedustPayout":           DedustPayout.opcode,
}

WITHDRAW_BLOCK_TYPES = {
    "dex_withdraw_liquidity": Block,
}


def register_dedust(reg: Registries) -> Registries:
    """Register everything specs/dedust_swap.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_builder("build_dedust_swap")
    async def build_dedust_swap(m):
        consumed = m._mch_consumed
        return await build_dedust_swap_core(consumed[0], consumed[1:])

    return reg


def dedust_registries() -> Registries:
    """Fresh Registries ready to compile specs/dedust_swap.mch."""
    return register_dedust(Registries())


def register_dedust_withdraw(reg: Registries) -> Registries:
    """Register everything specs/dedust_withdraw.mch needs into `reg`."""
    reg.opcodes.update(WITHDRAW_OPCODES)
    reg.block_types.update(WITHDRAW_BLOCK_TYPES)

    @reg.register_builder("build_dedust_withdraw")
    async def build_dedust_withdraw(m):
        return await build_dedust_withdraw_core(m.burn, [m.payout_1, m.payout_2])

    return reg


def dedust_withdraw_registries() -> Registries:
    """Fresh Registries ready to compile specs/dedust_withdraw.mch."""
    return register_dedust_withdraw(Registries())
