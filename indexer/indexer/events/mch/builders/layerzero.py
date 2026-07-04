"""mch builder for the LayerZero receive matcher (specs/layerzero.mch).

Ported from blocks/layerzero.py LayerZeroReceiveMatcher. The legacy build reads a
single labeled block ("execute_callback") out of its consumed set and delegates
the rest to `build_layerzero_receive_core` (shared with the legacy matcher).

DUP-LABEL MAPPING (spec header explains why there are two capture names): the
legacy pattern reuses the label "execute_callback" on two nested non-exclusive
nodes — LayerzeroLzReceiveExecute (0x0c7b8418, layerzero.py:363) and
LayerZeroOappExecuteCallback (0xcaae25a1, layerzero.py:368). Same-name captures
are only legal across exclusive branches (R005), so the spec gives them distinct
names: @execute (outer) and @oapp_callback (inner). The legacy `get_labeled`
returns the FIRST label in traversal order — the OUTER one — so this builder
passes `m.execute` to the core. `m.oapp_callback` (the inner OApp callback) binds
for pattern fidelity but the legacy build never read it; verified against both
corpus fixtures.

Captures bind as scalars (the pattern has no recursion). The anchor block is
`m.prepare` (the Channel LZ_RECEIVE_PREPARE); the core reads
`block.get_message().source` off it for the receive sender.
"""
from __future__ import annotations

from indexer.events.blocks.layerzero import (
    LayerZeroReceiveBlock,
    build_layerzero_receive_core,
)
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


def register_layerzero_receive(reg: Registries) -> Registries:
    """Register everything specs/layerzero.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_builder("build_layerzero_receive")
    async def build_layerzero_receive(m):
        # @execute is the OUTER LayerzeroLzReceiveExecute — the block the legacy
        # get_labeled("execute_callback") resolves to. @oapp_callback (inner) is
        # bound but intentionally unused, matching the legacy build. The core
        # returns None to reject (spec §10.7); the synthesized wrapper does the
        # merge_blocks, so the core must not.
        return build_layerzero_receive_core(m.prepare, m.execute)

    return reg


def layerzero_receive_registries() -> Registries:
    """Fresh Registries ready to compile specs/layerzero.mch."""
    return register_layerzero_receive(Registries())
