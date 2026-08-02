"""mch registration for the vesting family (specs/vesting.mch): the
send-message matcher and the add-whitelist one.

specs/vesting.mch is FULLY DECLARATIVE (build statements + `out {...}`, no
registered builder): the runtime evaluates it, so this module registers the
message parsers (for `parse`) plus the opcodes and produced block types, plus ONE
host fn for the genuinely inexpressible piece:

  vesting_message_was_sent — the `success` flag (upstream #430). Deciding
    whether the vesting contract actually emitted the requested message means
    scanning the REQUEST TRANSACTION's out-messages for a matching
    destination + body hash; transaction-level navigation is outside the
    language, so it stays a host fn wrapping the legacy
    _vesting_message_was_sent (blocks/vesting.py) verbatim.

`vesting_add_whitelist` needs neither a host fn nor a twin class: its `out` is
four field reads, and the legacy VestingAddWhiteList parser already yields plain
ints and pytoniq Addresses (the spec's `account(...)` map normalizes those to the
AccountId list the serializer wants).

The legacy VestingSendMessageBlockMatcher / VestingAddWhiteListBlockMatcher and
build_vesting_send_message_core (blocks/vesting.py) are unchanged and remain the
parity reference.
"""
from __future__ import annotations

from indexer.events.blocks.core import Block
from indexer.events.blocks.messages.vesting import (
    VestingAddWhiteList,
    VestingSendMessage,
)
from indexer.events.blocks.vesting import (
    VestingSendMessageBlock,
    _vesting_message_was_sent,
)
from indexer.events.mch.registry import Registries

OPCODES = {
    "VestingSendMessage":         VestingSendMessage.opcode,
    "VestingSendMessageResponse": VestingSendMessage.response_opcode,
    "VestingAddWhiteList":         VestingAddWhiteList.opcode,
    "VestingAddWhiteListResponse": VestingAddWhiteList.response_opcode,
}

BLOCK_TYPES = {
    "VestingSendMessageBlock": VestingSendMessageBlock,
    # Declarative producer: snake_case btype, generic Block (the IR engine stamps
    # the name straight onto the block it constructs).
    "vesting_add_whitelist": Block,
}

MESSAGE_TYPES = {
    "VestingSendMessage": VestingSendMessage,
    "VestingAddWhiteList": VestingAddWhiteList,
}


async def _vesting_message_was_sent_fn(send, body) -> bool:
    """`success`: did the vesting contract actually emit the requested message?
    Delegates to the legacy _vesting_message_was_sent over the request
    transaction, which the language cannot navigate."""
    if send is None or body is None:
        return False
    return _vesting_message_was_sent(send.get_message().transaction, body)


def register_vesting(reg: Registries) -> Registries:
    """Register everything specs/vesting.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)
    reg.message_types.update(MESSAGE_TYPES)
    reg.register_fn("vesting_message_was_sent")(_vesting_message_was_sent_fn)

    return reg


def vesting_registries() -> Registries:
    """Fresh Registries ready to compile specs/vesting.mch."""
    return register_vesting(Registries())
