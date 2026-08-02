"""MCH registrations for the LayerZero family in specs/layerzero.mch.

The runtime evaluates the spec's parse, reject, and output expressions
parse/reject/out for all five matchers, so this module only registers the
opcodes, the produced block types, the message parsers, and the single host
`fn` the declarative build cannot express.

Host function:

- layerzero_dst_oapp_matches implements `dest[2:].lower() ==
  dst_oapp[2:].lower()`. The envelope destination is a TON address (`0:<hex>`)
  and the packet dst_oapp is a bare 256-bit hash (`0x<hex>`); `account()` rejects
  the `0x` form (AddressError), and the expression language has no string
  manipulation, so prefix-strip + case-insensitive hex compare stays here.
  Byte-identical to build_layerzero_receive_core's guard.

Message parsers are `LayerZeroOappExecuteCallback` (receive),
`LayerzeroChannelSendCallback` (send), `ChannelCommitPacket`
(commit) and `UlnConnectionVerifyCallbackParser` (dvn verify). The last one is
also where the verification-status code becomes the status STRING, since the
expression language has no string operations.

The legacy matchers in blocks/layerzero.py stay the parity reference.
"""
from __future__ import annotations

from indexer.events.blocks.layerzero import (
    LayerZeroCommitPacketBlock,
    LayerZeroDvnVerifyBlock,
    LayerZeroReceiveBlock,
    LayerZeroSendBlock,
    LayerZeroSendTokensBlock,
)
from indexer.events.blocks.messages.layerzero import (
    ChannelChannelSend,
    ChannelCommitPacket,
    ChannelLzReceiveLock,
    ChannelLzReceivePrepare,
    ChannelMsglibSendCallback,
    DvnVerify,
    EndpointEndpointCommitPacket,
    EndpointEndpointSend,
    LayerZeroEventMsgBody,
    LayerZeroOappExecuteCallback,
    LayerzeroChannelSendCallback,
    LayerzeroLzReceiveExecute,
    LayerzeroLzReceivePrepare,
    MsglibconnectionMsglibConnectionCommitPacketCallback,
    MsglibconnectionMsglibConnectionSend,
    ProxyCallContract,
    UlnConnectionVerifyCallbackParser,
    UlnUlnCommitPacket,
    UlnUlnSend,
    UlnUlnVerify,
    UlnconnectionUlnConnectionCommitPacket,
    UlnconnectionUlnConnectionVerify,
)
from indexer.events.mch.registry import Registries

OPCODES = {
    "ChannelLzReceivePrepare":      ChannelLzReceivePrepare.opcode,
    "LayerzeroLzReceivePrepare":    LayerzeroLzReceivePrepare.opcode,
    "ChannelLzReceiveLock":         ChannelLzReceiveLock.opcode,
    "LayerzeroLzReceiveExecute":    LayerzeroLzReceiveExecute.opcode,
    "LayerZeroOappExecuteCallback": LayerZeroOappExecuteCallback.opcode,
    "LayerZeroEventMsgBody":        LayerZeroEventMsgBody.opcode,

    "EndpointEndpointSend":         EndpointEndpointSend.opcode,
    "ChannelChannelSend":           ChannelChannelSend.opcode,
    "MsglibConnectionSend":         MsglibconnectionMsglibConnectionSend.opcode,
    "UlnUlnSend":                   UlnUlnSend.opcode,
    "ChannelMsglibSendCallback":    ChannelMsglibSendCallback.opcode,
    "LayerzeroChannelSendCallback": LayerzeroChannelSendCallback.opcode,

    "UlnUlnCommitPacket":                  UlnUlnCommitPacket.opcode,
    "UlnConnectionCommitPacket":           UlnconnectionUlnConnectionCommitPacket.opcode,
    "EndpointEndpointCommitPacket":        EndpointEndpointCommitPacket.opcode,
    "ChannelCommitPacket":                 ChannelCommitPacket.opcode,
    "MsglibConnectionCommitPacketCallback":
        MsglibconnectionMsglibConnectionCommitPacketCallback.opcode,

    "DvnVerify":                   DvnVerify.opcode,
    "ProxyCallContract":           ProxyCallContract.opcode,
    "UlnUlnVerify":                UlnUlnVerify.opcode,
    "UlnConnectionVerify":         UlnconnectionUlnConnectionVerify.opcode,
    "UlnConnectionVerifyCallback": UlnConnectionVerifyCallbackParser.opcode,
}

BLOCK_TYPES = {
    "LayerZeroReceiveBlock":      LayerZeroReceiveBlock,
    "LayerZeroSendBlock":         LayerZeroSendBlock,
    "LayerZeroSendTokensBlock":   LayerZeroSendTokensBlock,
    "LayerZeroCommitPacketBlock": LayerZeroCommitPacketBlock,
    "LayerZeroDvnVerifyBlock":    LayerZeroDvnVerifyBlock,
}

MESSAGE_TYPES = {
    "LayerZeroOappExecuteCallback":      LayerZeroOappExecuteCallback,
    "LayerzeroChannelSendCallback":      LayerzeroChannelSendCallback,
    "ChannelCommitPacket":               ChannelCommitPacket,
    "UlnConnectionVerifyCallbackParser": UlnConnectionVerifyCallbackParser,
}


def register_layerzero(reg: Registries) -> Registries:
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


def layerzero_registries() -> Registries:
    """Fresh Registries ready to compile specs/layerzero.mch."""
    return register_layerzero(Registries())
