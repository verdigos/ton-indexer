"""mch host fn for the stonfi v1 swap matcher (specs/stonfi_v1_swap.mch, R12).

specs/stonfi_v1_swap.mch is a HYBRID declarative build: the pattern, failed
flag and out-field mapping live in the .mch, and the imperative residue is the
single host fn below. It delegates to the shared core
`build_stonfi_v1_swap_core` (blocks/swaps.py). The core needs the payment
requests the LEGACY chain would consume — the swap call's payment-request
children that carry a jetton_transfer child, including the ok_ref referral one
the mch pattern deliberately does NOT consume — so they are reconstructed from
the tree (`swap.next_blocks`, tree order == legacy consumption order for
children of one block). The core's referral_request_blocks return is dropped:
those blocks were never consumed here, which is the whole point of the remake
(legacy un-consumes them by bfs-removal in `_get_block_data`).
"""
from __future__ import annotations

from types import SimpleNamespace

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block
from indexer.events.blocks.messages import StonfiPaymentRequest, StonfiSwapMessage
from indexer.events.blocks.swaps import (
    JettonSwapBlock,
    build_stonfi_v1_swap_core,
    stonfi_sender_related_exit_codes,
)
from indexer.events.mch.registry import Registries

OPCODES = {
    "StonfiSwap":           StonfiSwapMessage.opcode,
    "StonfiPaymentRequest": StonfiPaymentRequest.opcode,
}

BLOCK_TYPES = {
    "JettonSwapBlock": JettonSwapBlock,
}


async def _stonfi_v1_swap_data(in_transfer: Block, swap: Block) -> SimpleNamespace:
    """Imperative residue of the stonfi v1 swap build: reconstruct the payment
    requests the legacy chain would consume, delegate to the shared core, and
    expose its data dict as a namespace (`.success` drives the failed flag).
    The referral_request_blocks return is dropped — those blocks are never
    consumed by the mch pattern (the whole point of the remake)."""
    payment_requests_messages = [
        (StonfiPaymentRequest(x.get_body()), x)
        for x in swap.next_blocks
        if isinstance(x, CallContractBlock)
        and x.opcode == StonfiPaymentRequest.opcode
        and any(nb.btype == 'jetton_transfer' for nb in x.next_blocks)
    ]
    data, _referral_request_blocks = await build_stonfi_v1_swap_core(
        in_transfer, swap, payment_requests_messages)
    return SimpleNamespace(**data)


def register_stonfi_v1(reg: Registries) -> Registries:
    """Register everything specs/stonfi_v1_swap.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_predicate("stonfi_v1_sender_payment")
    def stonfi_v1_sender_payment(block: Block) -> bool:
        # Sender-related exit codes (ok / no_liq / reserve_err); the ok_ref
        # referral payment fails this gate and stays unconsumed. The engine
        # only calls it after the opcode check, where the body parse is total;
        # the try/except guards the capture walker's speculative probes.
        try:
            return StonfiPaymentRequest(block.get_body()).exit_code \
                in stonfi_sender_related_exit_codes
        except Exception:
            return False

    reg.register_fn("stonfi_v1_swap_data")(_stonfi_v1_swap_data)

    return reg


def stonfi_v1_registries() -> Registries:
    """Fresh Registries ready to compile specs/stonfi_v1_swap.mch."""
    return register_stonfi_v1(Registries())
