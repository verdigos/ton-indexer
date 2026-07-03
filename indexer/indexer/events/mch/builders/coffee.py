"""mch builder for the coffee swap matcher (specs/coffee_swap.mch).

The legacy build (blocks/swaps.py build_coffee_swap_core) needs the incoming
transfer, the CoffeePayoutInternal and the outgoing transfer, plus the full
consumed set for find_call_contracts. The legacy matcher extracts the three
via labels; the mch engine has no labels and captures bind only on the
zero-hop path (see roadmap T15 notes), so this builder derives them
structurally from `Match._mch_consumed`:

- in_transfer: the anchor's previous_block (the anchor's parent alternative is
  required, so the block is always consumed when the match succeeds);
- payout: the single CoffeePayoutInternal in the consumed set (the recursion
  exit fires exactly once per match);
- out_transfer: the consumed child of the payout, first in consumed order —
  matching legacy get_labeled semantics when several payout children matched.

Returns None to reject the match (spec §10.7).
"""
from __future__ import annotations

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.messages.coffee import (
    CoffeeNotification,
    CoffeePayout,
    CoffeePayoutInternal,
    CoffeeSwapInternal,
    CoffeeSwapNative,
    CoffeeSwapSuccessfulEvent,
)
from indexer.events.blocks.swaps import JettonSwapBlock, build_coffee_swap_core
from indexer.events.mch.registry import Registries

OPCODES = {
    "CoffeeSwapNative":          CoffeeSwapNative.opcode,
    "CoffeeSwapInternal":        CoffeeSwapInternal.opcode,
    "CoffeeSwapSuccessfulEvent": CoffeeSwapSuccessfulEvent.opcode,
    "CoffeePayoutInternal":      CoffeePayoutInternal.opcode,
    "CoffeePayout":              CoffeePayout.opcode,
    "CoffeeNotification":        CoffeeNotification.opcode,
}

BLOCK_TYPES = {
    "JettonSwapBlock": JettonSwapBlock,
}

_OUT_OPCODES = (CoffeePayout.opcode, CoffeeNotification.opcode)


def register_coffee(reg: Registries) -> Registries:
    """Register everything specs/coffee_swap.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_builder("build_coffee_swap")
    async def build_coffee_swap(m):
        consumed = m._mch_consumed
        block, others = consumed[0], list(consumed[1:])

        in_transfer = None
        prev = block.previous_block
        if prev is not None and prev in others:
            if prev.btype == "jetton_transfer" or (
                isinstance(prev, CallContractBlock)
                and prev.opcode == CoffeeSwapNative.opcode
            ):
                in_transfer = prev

        payout = next(
            (
                b for b in others
                if isinstance(b, CallContractBlock)
                and b.opcode == CoffeePayoutInternal.opcode
            ),
            None,
        )

        out_transfer = None
        if payout is not None:
            for b in others:
                if b.previous_block is payout and (
                    b.btype == "jetton_transfer"
                    or (isinstance(b, CallContractBlock) and b.opcode in _OUT_OPCODES)
                ):
                    out_transfer = b
                    break

        return await build_coffee_swap_core(
            block, others, in_transfer, payout, out_transfer
        )

    return reg


def coffee_registries() -> Registries:
    """Fresh Registries ready to compile specs/coffee_swap.mch."""
    return register_coffee(Registries())
