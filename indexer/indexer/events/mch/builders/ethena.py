"""mch registration for the ethena family (specs/ethena.mch): the deposit
matcher and the withdrawal-request one.

Both are FULLY DECLARATIVE — no builder, no host fn. The deposit's hardcoded
USDe/tsUSDe asset gate is `reject when` clauses over the composed
jetton_transfer / jetton_mint blocks' `data`; the withdrawal request's four
identity guards are `reject when`s over two `lookup jetton_wallet(...)` records
(the first interface lookups in this spec), and its two body reads reuse the
JettonInternalTransfer schema entry — including for the 0xb2583ed5 timelocked
transfer, whose body IS an internal transfer (every parser skips its leading
opcode unchecked, and `parse` is keyed by registry NAME, not opcode).

So this module only registers opcodes, message types and produced block types.
"""
from __future__ import annotations

from indexer.events.blocks.core import Block
from indexer.events.blocks.ethena_staking import EthenaDepositBlock
from indexer.events.blocks.messages import JettonMint, JettonTransfer
from indexer.events.blocks.messages.jettons import (
    JettonInternalTransfer,
    JettonNotify,
)
from indexer.events.mch.registry import Registries, default_lookups

# 0xb2583ed5 has no legacy message class of its own — the reference parses it
# with JettonInternalTransfer — so the opcode is spelled out here.
ETHENA_TIMELOCK_TRANSFER_OPCODE = 0xB2583ED5

OPCODES = {
    "JettonTransfer":         JettonTransfer.opcode,
    "JettonInternalTransfer": JettonInternalTransfer.opcode,
    "JettonNotify":           JettonNotify.opcode,
    "JettonMint":             JettonMint.opcode,
    "EthenaTimelockTransfer": ETHENA_TIMELOCK_TRANSFER_OPCODE,
}

MESSAGE_TYPES = {
    "JettonInternalTransfer": JettonInternalTransfer,
}

BLOCK_TYPES = {
    "EthenaDepositBlock": EthenaDepositBlock,
    # Declarative producer: snake_case btype, generic Block.
    "ethena_withdrawal_request": Block,
}


def register_ethena(reg: Registries) -> Registries:
    """Register everything specs/ethena.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.message_types.update(MESSAGE_TYPES)
    reg.block_types.update(BLOCK_TYPES)
    # The withdrawal request's two `lookup jetton_wallet(...)` guards.
    reg.lookups.update(default_lookups())
    return reg


def ethena_registries() -> Registries:
    """Fresh Registries ready to compile specs/ethena.mch."""
    return register_ethena(Registries())
