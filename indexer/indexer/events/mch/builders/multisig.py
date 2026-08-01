"""Registries for the fully declarative multisig spec.

The four matchers in specs/multisig.mch use only structural patterns, generated
message parsers, build expressions, and one nullable MultisigOrder lookup. The
approve comment arm is split from the opcode arm so its comment guard stays an
inline expression and its always-failing body parse is represented by the
legacy literal ``signer_index = -1``. No host fn, predicate, shaper, or custom
builder is registered here.

The legacy matchers and message classes remain unchanged as the parity target.
"""
from __future__ import annotations

from typing import Any

from indexer.events import context
from indexer.events.blocks.core import Block
from indexer.events.blocks.messages.multisig import (
    MultisigApprove,
    MultisigApproveAccepted,
    MultisigApproveRejected,
    MultisigExecute,
    MultisigInitOrder,
    MultisigNewOrder,
)
from indexer.events.mch.registry import Registries

OPCODES = {
    "MultisigNewOrder":       MultisigNewOrder.opcode,
    "MultisigInitOrder":      MultisigInitOrder.opcode,
    "MultisigApprove":        MultisigApprove.opcode,
    "MultisigApproveAccepted": MultisigApproveAccepted.opcode,
    "MultisigApproveRejected": MultisigApproveRejected.opcode,
    "MultisigExecute":        MultisigExecute.opcode,
}

BLOCK_TYPES = {
    "multisig_create_order": Block,
    "multisig_approve": Block,
    "multisig_execute": Block,
}

MESSAGE_TYPES = {
    "MultisigNewOrder": MultisigNewOrder,
    "MultisigInitOrder": MultisigInitOrder,
    "MultisigApprove": MultisigApprove,
    "MultisigApproveRejected": MultisigApproveRejected,
    "MultisigExecute": MultisigExecute,
}


def register_multisig(reg: Registries) -> Registries:
    """Register everything specs/multisig.mch needs into ``reg``."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)
    reg.message_types.update(MESSAGE_TYPES)

    @reg.register_lookup("multisig_order")
    async def multisig_order(address: str) -> Any:
        repo = context.interface_repository.get()
        return await repo.get_multisig_order(address) if repo is not None else None

    return reg


def multisig_registries() -> Registries:
    """Fresh Registries ready to compile specs/multisig.mch."""
    return register_multisig(Registries())
