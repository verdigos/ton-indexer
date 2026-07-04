"""mch builder for the tonco swap matcher (specs/tonco_swap.mch, R11 = T28).

The legacy build (blocks/swaps.py ToncoSwapBlockMatcher.build_block) works off
LabelBlocks, which the mch engine does not emit — and captures cannot live
inside a cyclic rule body (C012), so this builder reconstructs every label
role structurally from `Match._mch_consumed` and delegates to the shared core
`build_tonco_swap_core`.

The core's additional_blocks (pton transfer calls beside the pattern) are
ignored here: the pattern already consumes them (`{ maybe op PTonTransfer }`
on the final-jetton branch, the in-chain pton on the hop branch), so the
engine's merge covers them.

NOT to be confused with builders/tonco.py — that one is tonco LIQUIDITY.
"""
from __future__ import annotations

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block
from indexer.events.blocks.messages import JettonNotify, JettonTransfer, PTonTransfer
from indexer.events.blocks.swaps import (
    JettonSwapBlock,
    ToncoSwapBlockMatcher,
    build_tonco_swap_core,
)
from indexer.events.mch.registry import Registries

OPCODES = {
    "ToncoPoolV3Swap":    ToncoSwapBlockMatcher.swap_opcode,
    "ToncoRouterV3PayTo": ToncoSwapBlockMatcher.pay_to_opcode,
    "JettonNotify":       JettonNotify.opcode,
    "JettonTransfer":     JettonTransfer.opcode,
    "PTonTransfer":       PTonTransfer.opcode,
}

BLOCK_TYPES = {
    "JettonSwapBlock": JettonSwapBlock,
}


def _is_call(b: Block, opcode: int) -> bool:
    return isinstance(b, CallContractBlock) and b.opcode == opcode


def _derive_parts(consumed: list[Block]):
    """Reconstruct the legacy label roles from the consumed set.

    peer_swap: every consumed POOLV3_SWAP except the anchor; payout: every
    consumed ROUTERV3_PAY_TO; in_transfer: the anchor's previous block (jetton
    arm) or that block's previous (JettonNotify <- PTonTransfer arm). Payout
    children split by whether they provably continue to a consumed swap
    (intermediate_transfer) or not (out_transfer) — the same look-down-the-
    chain test the cyclic pattern's branch order encodes. hop triggers are the
    consumed JettonNotify calls followed by a consumed non-anchor swap (the
    in-arm notify is followed by the anchor itself). All lists ascend by
    min_lt, mirroring the legacy label scan over lt-sorted other_blocks.
    """
    anchor = consumed[0]
    swap_op = ToncoSwapBlockMatcher.swap_opcode
    pay_op = ToncoSwapBlockMatcher.pay_to_opcode
    by_lt = lambda b: b.min_lt  # noqa: E731

    swaps = [b for b in consumed if _is_call(b, swap_op)]
    swap_ids = set(map(id, swaps))
    payouts = sorted((b for b in consumed if _is_call(b, pay_op)), key=by_lt)
    payout_ids = set(map(id, payouts))
    peer_swaps = sorted((b for b in swaps if b is not anchor), key=by_lt)

    prev = anchor.previous_block
    in_transfer = None
    if prev is not None:
        in_transfer = prev if prev.btype == "jetton_transfer" else prev.previous_block

    def leads_to_hop(b: Block) -> bool:
        for c in b.next_blocks:
            if id(c) in swap_ids:
                return True
            if _is_call(c, PTonTransfer.opcode):
                for cc in c.next_blocks:
                    if _is_call(cc, JettonNotify.opcode) and any(
                        id(g) in swap_ids for g in cc.next_blocks
                    ):
                        return True
        return False

    transfers = [
        b for b in consumed
        if b.previous_block is not None and id(b.previous_block) in payout_ids
        and (b.btype == "jetton_transfer" or _is_call(b, JettonTransfer.opcode))
    ]
    intermediate_transfers = sorted((b for b in transfers if leads_to_hop(b)), key=by_lt)
    out_transfers = sorted((b for b in transfers if not leads_to_hop(b)), key=by_lt)

    intermediate_hop_triggers = sorted(
        (b for b in consumed
         if _is_call(b, JettonNotify.opcode)
         and any(id(g) in swap_ids and g is not anchor for g in b.next_blocks)),
        key=by_lt,
    )

    return (anchor, in_transfer, peer_swaps, payouts, out_transfers,
            intermediate_transfers, intermediate_hop_triggers)


def register_tonco_swap(reg: Registries) -> Registries:
    """Register everything specs/tonco_swap.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_builder("build_tonco_swap")
    async def build_tonco_swap(m):
        core = await build_tonco_swap_core(*_derive_parts(m._mch_consumed))
        if core is None:
            return None
        new_block, _additional = core
        return new_block

    return reg


def tonco_swap_registries() -> Registries:
    """Fresh Registries ready to compile specs/tonco_swap.mch."""
    return register_tonco_swap(Registries())
