"""MCH registries for the Stonfi V2 provide-liquidity matchers
(specs/stonfi_v2_provide.mch).

Each matcher builds one deposit leg's `dex_deposit_liquidity` data in the
spec's `out` block. The product
is a generic Block(btype dex_deposit_liquidity); cross-branch pairing, the
EmptyBlock proxy surgery and condition-fail absorption live in
the host pass post_process_stonfi_v2_liquidity (blocks/liquidity.py), which
accepts either the source classes or a btype+dex marker.

The registry therefore needs only the opcode constants, the produced btype in
block_types, and the two message parsers the `parse` statements reference.
"""
from __future__ import annotations

from indexer.events.blocks.core import Block
from indexer.events.blocks.messages.jettons import JettonInternalTransfer
from indexer.events.blocks.messages.liquidity import StonfiV2ProvideLiquidity
from indexer.events.mch.registry import Registries

OPCODES = {
    "StonfiV2ProvideLiquidity": 0x37C096DF,
    "StonfiV2AddLiquidity":     0x50C6A654,
    "StonfiV2CbAddLiquidity":   0x06ECD527,
    "JettonNotify":             0x7362D09C,
    "JettonInternalTransfer":   0x178D4519,
    "PTonTransfer":             0x01F3835D,
}

BLOCK_TYPES = {
    "dex_deposit_liquidity": Block,
}

MESSAGE_TYPES = {
    "StonfiV2ProvideLiquidity": StonfiV2ProvideLiquidity,
    "JettonInternalTransfer":   JettonInternalTransfer,
}


def register_stonfi_provide(reg: Registries) -> Registries:
    """Register everything specs/stonfi_v2_provide.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)
    reg.message_types.update(MESSAGE_TYPES)
    return reg


def stonfi_provide_registries() -> Registries:
    """Fresh Registries ready to compile specs/stonfi_v2_provide.mch."""
    return register_stonfi_provide(Registries())
