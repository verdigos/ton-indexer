"""mch builders for the jetton matcher family (specs/jettons.mch).

Thin adapters: extract captured blocks from the runtime Match and delegate to
the shared build cores in blocks/jettons.py — the same functions the legacy
BlockMatchers call. Builders return the produced block (the synthesized
wrapper merges consumed blocks) or None to reject the match (spec §10.7).
"""
from __future__ import annotations

from indexer.events.blocks.jettons import (
    JettonBurnBlock,
    JettonMintBlock,
    JettonTransferBlock,
    build_jetton_burn_core,
    build_jetton_mint_core,
    build_jetton_transfer_core,
)
from indexer.events.blocks.messages import (
    JettonBurn,
    JettonBurnNotification,
    JettonInternalTransfer,
    JettonMint,
    JettonNotify,
    JettonTransfer,
)
from indexer.events.blocks.messages.jettons import MinterJettonMint
from indexer.events.mch.registry import Registries

OPCODES = {
    "JettonTransfer":         JettonTransfer.opcode,
    "JettonInternalTransfer": JettonInternalTransfer.opcode,
    "JettonNotify":           JettonNotify.opcode,
    "JettonBurn":             JettonBurn.opcode,
    "JettonBurnNotification": JettonBurnNotification.opcode,
    "JettonMint":             JettonMint.opcode,
    "MinterJettonMint":       MinterJettonMint.opcode,
}

BLOCK_TYPES = {
    "JettonTransferBlock": JettonTransferBlock,
    "JettonBurnBlock":     JettonBurnBlock,
    "JettonMintBlock":     JettonMintBlock,
}


def register_jettons(reg: Registries) -> Registries:
    """Register everything specs/jettons.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_builder("build_jetton_transfer")
    async def build_jetton_transfer(m):
        return await build_jetton_transfer_core(m.transfer, m.internal)

    @reg.register_builder("build_jetton_burn")
    async def build_jetton_burn(m):
        return await build_jetton_burn_core(m.burn)

    @reg.register_builder("build_jetton_mint")
    async def build_jetton_mint(m):
        return await build_jetton_mint_core(m.mint, m.internal)

    return reg


def jetton_registries() -> Registries:
    """Fresh Registries ready to compile specs/jettons.mch."""
    return register_jettons(Registries())
