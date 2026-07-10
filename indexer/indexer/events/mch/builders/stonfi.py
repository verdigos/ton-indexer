"""mch host fn for the stonfi_v2 swap matcher (specs/stonfi_v2_swap.mch).

specs/stonfi_v2_swap.mch is a HYBRID declarative build: the pattern, failed
flag and out-field mapping live in the .mch, and the imperative residue is the
single host fn below plus the two predicates. The legacy build (blocks/swaps.py
StonfiV2SwapBlockMatcher.build_block) works off LabelBlocks, which the mch
engine does not emit; `_derive_parts` reconstructs the label roles structurally
from the consumed set (it predates the T18 capture fixes — spec §7.2 — and the
consumed set remains the simplest faithful input for the legacy scan logic),
then delegates to the shared core `build_stonfi_v2_swap_core`.

The core's additional_blocks_to_include (terminal pton transfer call) is
ignored here: the pattern already consumes that block via the
`{ maybe op PTonTransfer }` children set, so the synthesized merge covers it.
"""
from __future__ import annotations

from types import SimpleNamespace

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block
from indexer.events.blocks.messages import JettonNotify, PTonTransfer
from indexer.events.blocks.swaps import (
    JettonSwapBlock,
    StonfiV2SwapBlockMatcher,
    build_stonfi_v2_swap_core,
    validate_stonfi_v2_intermediate_pton_transfer,
)
from indexer.events.mch.registry import Registries

OPCODES = {
    "StonfiV2Swap":  StonfiV2SwapBlockMatcher.swap_opcode,
    "StonfiV2PayTo": StonfiV2SwapBlockMatcher.pay_to_opcode,
    "JettonNotify":  JettonNotify.opcode,
    "PTonTransfer":  PTonTransfer.opcode,
}

BLOCK_TYPES = {
    "JettonSwapBlock": JettonSwapBlock,
}


def _derive_parts(consumed: list[Block]):
    """Reconstruct the legacy label roles from the consumed set.

    peer_swap/payout: each swap request's pay_to is its direct child;
    in_transfer: the anchor's previous block (jetton arm) or that block's
    previous (JettonNotify <- PTonTransfer arm); out_transfer: the last (by lt)
    consumed jetton_transfer hanging off a consumed pay_to — same block the
    legacy label scan keeps after its overwrite-in-lt-order loop.
    """
    anchor = consumed[0]
    # `_mch_consumed` arrives de-duplicated since T18 (spec §11.1), so no
    # (swap, payout) pair can double up here.
    payouts = [b for b in consumed if isinstance(b, CallContractBlock)
               and b.opcode == StonfiV2SwapBlockMatcher.pay_to_opcode]
    swaps = sorted((b for b in consumed if isinstance(b, CallContractBlock)
                    and b.opcode == StonfiV2SwapBlockMatcher.swap_opcode),
                   key=lambda b: b.min_lt)
    peer_swap_blocks = [(s, next(p for p in s.next_blocks if p in payouts))
                        for s in swaps]
    prev = anchor.previous_block
    in_transfer = prev if prev.btype == 'jetton_transfer' else prev.previous_block
    outs = [b for b in consumed if b.btype == 'jetton_transfer'
            and b.previous_block in payouts]
    out_transfer = max(outs, key=lambda b: b.min_lt) if outs else None
    return anchor, peer_swap_blocks, in_transfer, out_transfer


async def _stonfi_v2_swap_data(consumed: list[Block]) -> SimpleNamespace:
    """Imperative residue of the stonfi_v2 swap build: reconstruct the leg
    roles from the consumed set, delegate to the shared core, and expose its
    data dict as a namespace (`.failed` drives the failed flag). The core's
    additional_blocks_to_include is dropped — the pattern already consumes the
    terminal pton call. The core sets `anchor.broken` on a wallet-lookup miss;
    that flag rides on a merged child, exactly as the legacy build leaves it
    (merge_blocks does not propagate it to the produced swap block)."""
    anchor, peer_swap_blocks, in_transfer, out_transfer = _derive_parts(consumed)
    new_block, _additional = await build_stonfi_v2_swap_core(
        anchor, peer_swap_blocks, in_transfer, out_transfer)
    return SimpleNamespace(failed=new_block.failed, **new_block.data)


def register_stonfi_v2(reg: Registries) -> Registries:
    """Register everything specs/stonfi_v2_swap.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_predicate("pton_self_transfer")
    def pton_self_transfer(block: Block) -> bool:
        # Exact legacy decision logic. The try/except only guards the capture
        # walker's speculative probes (arbitrary consumed blocks); the engine
        # only ever tests children of a matched jetton_transfer block, where
        # the legacy function is total.
        try:
            return validate_stonfi_v2_intermediate_pton_transfer(block)
        except Exception:
            return False

    @reg.register_predicate("no_internal_transfer")
    def no_internal_transfer(block: Block) -> bool:
        return not block.data['has_internal_transfer']

    reg.register_fn("stonfi_v2_swap_data")(_stonfi_v2_swap_data)

    return reg


def stonfi_v2_registries() -> Registries:
    """Fresh Registries ready to compile specs/stonfi_v2_swap.mch."""
    return register_stonfi_v2(Registries())
