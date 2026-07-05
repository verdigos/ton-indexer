"""mch registration for the vesting send-message matcher (specs/vesting.mch).

specs/vesting.mch is FULLY DECLARATIVE (build statements + `out {...}`, no
registered builder): the runtime evaluates it, so this module registers the
message parser (for `parse`) plus the opcodes and produced block type. The
legacy VestingSendMessageBlockMatcher and its build_vesting_send_message_core
(blocks/vesting.py) are unchanged and remain the parity reference.
"""
from __future__ import annotations

from indexer.events.blocks.messages.vesting import VestingSendMessage
from indexer.events.blocks.vesting import VestingSendMessageBlock
from indexer.events.mch.registry import Registries

OPCODES = {
    "VestingSendMessage":         VestingSendMessage.opcode,
    "VestingSendMessageResponse": VestingSendMessage.response_opcode,
}

BLOCK_TYPES = {
    "VestingSendMessageBlock": VestingSendMessageBlock,
}

MESSAGE_TYPES = {
    "VestingSendMessage": VestingSendMessage,
}


def register_vesting(reg: Registries) -> Registries:
    """Register everything specs/vesting.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)
    reg.message_types.update(MESSAGE_TYPES)

    return reg


def vesting_registries() -> Registries:
    """Fresh Registries ready to compile specs/vesting.mch."""
    return register_vesting(Registries())
