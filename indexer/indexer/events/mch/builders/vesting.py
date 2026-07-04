"""mch builder for the vesting send-message matcher (specs/vesting.mch).

Thin adapter: extracts the anchor block from the runtime Match and delegates to
the shared build core in blocks/vesting.py — the same function the legacy
VestingSendMessageBlockMatcher calls. The synthesized wrapper merges the
consumed blocks (request + response).
"""
from __future__ import annotations

from indexer.events.blocks.messages.vesting import VestingSendMessage
from indexer.events.blocks.vesting import (
    VestingSendMessageBlock,
    build_vesting_send_message_core,
)
from indexer.events.mch.registry import Registries

OPCODES = {
    "VestingSendMessage":         VestingSendMessage.opcode,
    "VestingSendMessageResponse": VestingSendMessage.response_opcode,
}

BLOCK_TYPES = {
    "VestingSendMessageBlock": VestingSendMessageBlock,
}


def register_vesting(reg: Registries) -> Registries:
    """Register everything specs/vesting.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_builder("build_vesting_send_message")
    async def build_vesting_send_message(m):
        return build_vesting_send_message_core(m.send)

    return reg


def vesting_registries() -> Registries:
    """Fresh Registries ready to compile specs/vesting.mch."""
    return register_vesting(Registries())
