"""Registries for specs/elections.mch (elector deposit / recover).

The thinnest builder in the tree: the two matchers never parse a body, never
look anything up and have no host residue, so all this registers is the four
elector opcodes and the two produced block types. The reference classes
(blocks/messages/staking.py Elector*) are bare opcode constants with no parser
at all, which is why there is no `message_types` row and no protocol ABI
entry — the spec matches them with `op` and reads only message envelopes.
"""
from __future__ import annotations

from indexer.events.blocks.core import Block
from indexer.events.blocks.messages import (
    ElectorDepositStakeConfirmation,
    ElectorDepositStakeRequest,
    ElectorRecoverStakeConfirmation,
    ElectorRecoverStakeRequest,
)
from indexer.events.mch.registry import Registries

ELECTION_OPCODES = {
    "ElectorDepositStakeRequest": ElectorDepositStakeRequest.opcode,
    "ElectorDepositStakeConfirmation": ElectorDepositStakeConfirmation.opcode,
    "ElectorRecoverStakeRequest": ElectorRecoverStakeRequest.opcode,
    "ElectorRecoverStakeConfirmation": ElectorRecoverStakeConfirmation.opcode,
}

ELECTION_BLOCK_TYPES = {
    "election_deposit": Block,
    "election_recover": Block,
}


def register_elections(reg: Registries) -> Registries:
    reg.opcodes.update(ELECTION_OPCODES)
    reg.block_types.update(ELECTION_BLOCK_TYPES)
    return reg


def elections_registries() -> Registries:
    """Fresh Registries ready to compile specs/elections.mch."""
    return register_elections(Registries())
