"""mch registries for the nominator_pool_withdraw matcher (specs/nominator.mch).

Remake (not a port) of blocks/staking.py NominatorPoolWithdrawMatcher — see the
spec file header for why the shape is re-anchored per payout instead of fanned
out from the NominatorPoolProcessWithdrawRequests call. FULLY DECLARATIVE build:
the produced fields are an `out {...}` block; the only host hooks are the
structural `nominator_pool_withdraw_parent` predicate (gates the match on the
parent call — a topology check that stays a host predicate, never a captured
node) and a nullable `nominator_pool` lookup (the NominatorPool interface-set
membership, G10 verdict). The shared build_nominator_pool_withdraw_core
(blocks/staking.py) stays with the legacy matcher on the production path only.
"""
from __future__ import annotations

from indexer.events import context
from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block
from indexer.events.blocks.messages.staking import NominatorPoolProcessWithdrawRequests
from indexer.events.mch.registry import Registries

BLOCK_TYPES = {
    "nominator_pool_withdraw_request": Block,
}


def register_nominator(reg: Registries) -> Registries:
    """Register everything specs/nominator.mch needs into `reg`."""
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_predicate("nominator_pool_withdraw_parent")
    def nominator_pool_withdraw_parent(block: Block) -> bool:
        parent = block.previous_block
        return (
            isinstance(parent, CallContractBlock)
            and parent.opcode == NominatorPoolProcessWithdrawRequests.opcode
        )

    @reg.register_lookup("nominator_pool")
    async def nominator_pool(address):
        # Nullable interface lookup (G10): resolves the payout's pool address to
        # its interface set, returning it only when the NominatorPool interface
        # is present (else None -> the build `reject when pool_iface == null`).
        repo = context.interface_repository.get()
        if repo is None:
            return None
        interfaces = await repo.get_interfaces(address)
        return interfaces if "NominatorPool" in interfaces else None

    return reg


def nominator_registries() -> Registries:
    """Fresh Registries ready to compile specs/nominator.mch."""
    return register_nominator(Registries())
