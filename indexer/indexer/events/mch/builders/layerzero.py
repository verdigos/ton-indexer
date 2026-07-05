"""mch registration for the LayerZero receive matcher (specs/layerzero.mch).

specs/layerzero.mch is DECLARATIVE (M4 §9): the runtime evaluates parse/reject/out,
so this module only registers the opcodes, the produced block type, the message
parser (for `parse execute as LayerZeroOappExecuteCallback`), and the single host
`fn` the declarative build cannot express.

Host `fn` (the only remaining dual-implementation liability):

- layerzero_dst_oapp_matches — the legacy safety check `dest[2:].lower() ==
  dst_oapp[2:].lower()`. The envelope destination is a TON address (`0:<hex>`)
  and the packet dst_oapp is a bare 256-bit hash (`0x<hex>`); `account()` rejects
  the `0x` form (AddressError), and the expression language has no string
  manipulation (§9.6), so prefix-strip + case-insensitive hex compare stays here.
  Byte-identical to build_layerzero_receive_core's guard.

The legacy LayerZeroReceiveMatcher and build_layerzero_receive_core stay the
parity reference.
"""
from __future__ import annotations

from indexer.events.blocks.layerzero import LayerZeroReceiveBlock
from indexer.events.blocks.messages.layerzero import (
    ChannelLzReceiveLock,
    ChannelLzReceivePrepare,
    LayerZeroEventMsgBody,
    LayerZeroOappExecuteCallback,
    LayerzeroLzReceiveExecute,
    LayerzeroLzReceivePrepare,
)
from indexer.events.mch.registry import Registries

OPCODES = {
    "ChannelLzReceivePrepare":      ChannelLzReceivePrepare.opcode,
    "LayerzeroLzReceivePrepare":    LayerzeroLzReceivePrepare.opcode,
    "ChannelLzReceiveLock":         ChannelLzReceiveLock.opcode,
    "LayerzeroLzReceiveExecute":    LayerzeroLzReceiveExecute.opcode,
    "LayerZeroOappExecuteCallback": LayerZeroOappExecuteCallback.opcode,
    "LayerZeroEventMsgBody":        LayerZeroEventMsgBody.opcode,
}

BLOCK_TYPES = {
    "LayerZeroReceiveBlock": LayerZeroReceiveBlock,
}

MESSAGE_TYPES = {
    "LayerZeroOappExecuteCallback": LayerZeroOappExecuteCallback,
}


def register_layerzero_receive(reg: Registries) -> Registries:
    """Register everything specs/layerzero.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)
    reg.message_types.update(MESSAGE_TYPES)

    @reg.register_fn("layerzero_dst_oapp_matches")
    async def layerzero_dst_oapp_matches(destination, dst_oapp) -> bool:
        # dest[2:].lower() == dst_oapp[2:].lower() — strip `0:` / `0x`, compare
        # the hex case-insensitively (see module docstring for why this can't be
        # `account(a) == account(b)`).
        if destination is None or dst_oapp is None:
            return False
        return str(destination)[2:].lower() == str(dst_oapp)[2:].lower()

    return reg


def layerzero_receive_registries() -> Registries:
    """Fresh Registries ready to compile specs/layerzero.mch."""
    return register_layerzero_receive(Registries())
