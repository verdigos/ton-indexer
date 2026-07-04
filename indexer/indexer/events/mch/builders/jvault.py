"""mch builders for the jvault family (specs/jvault.mch) — R02 remake.

Thin adapters over the shared cores in blocks/jvault.py (the same functions the
legacy JVault*BlockMatcher.build_block methods now call). Consumed-set-driven
(Match._mch_consumed), same convention as builders/evaa.py: the referral
recursion and the post-recursion `@update`/`@withdraw` positions do not bind
positionally (spec §7.2), and every legacy participant role carries a unique
opcode/btype within one match's consumed set, so `_derive_*` reconstructs the
roles unambiguously and delegates to the core.

Each core returns a single Block (or None to reject); the synthesized mch
build_block wraps it (merge_blocks([anchor] + other_blocks), return [produced]),
exactly like the legacy matcher's own wrapper.
"""
from __future__ import annotations

from indexer.events.blocks.basic_blocks import CallContractBlock, TonTransferBlock
from indexer.events.blocks.core import Block
from indexer.events.blocks.jettons import JettonTransferBlock
from indexer.events.blocks.jvault import (
    JVaultClaimBlock,
    JVaultStakeBlock,
    JVaultUnstakeBlock,
    JVaultUnstakeRequestBlock,
    build_jvault_claim_core,
    build_jvault_stake_core,
    build_jvault_unstake_core,
    build_jvault_unstake_request_core,
)
from indexer.events.blocks.messages.jvault import (
    JVaultRequestUpdateRewards,
    JVaultSendClaimedRewards,
)
from indexer.events.mch.registry import Registries

JVAULT_STAKE_CANCEL_OPCODE = 0x9EADA1D9

BLOCK_TYPES = {
    "JVaultStakeBlock": JVaultStakeBlock,
    "JVaultUnstakeBlock": JVaultUnstakeBlock,
    "JVaultClaimBlock": JVaultClaimBlock,
    "JVaultUnstakeRequestBlock": JVaultUnstakeRequestBlock,
}


def _first_call(consumed: list[Block], opcode: int) -> CallContractBlock | None:
    return next((b for b in consumed
                 if isinstance(b, CallContractBlock) and b.opcode == opcode), None)


def _first_jetton_transfer(consumed: list[Block]) -> JettonTransferBlock | None:
    return next((b for b in consumed if isinstance(b, JettonTransferBlock)), None)


def _first_ton_transfer(consumed: list[Block]) -> TonTransferBlock | None:
    return next((b for b in consumed if isinstance(b, TonTransferBlock)), None)


def register_jvault(reg: Registries) -> Registries:
    """Register everything specs/jvault.mch needs into `reg`."""
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_builder("build_jvault_stake")
    async def build_jvault_stake(m):
        consumed = m._mch_consumed
        anchor = consumed[0]  # the incoming jetton_transfer
        receive = _first_call(consumed, 0xD68A4AC1)  # JVaultReceiveJettons
        request_update = _first_call(consumed, JVaultRequestUpdateRewards.opcode)
        cancellation = _first_call(consumed, JVAULT_STAKE_CANCEL_OPCODE)
        return build_jvault_stake_core(anchor, receive, request_update, cancellation)

    @reg.register_builder("build_jvault_unstake")
    async def build_jvault_unstake(m):
        consumed = m._mch_consumed
        anchor = consumed[0]  # the unstake_jettons call
        request_update = _first_call(consumed, JVaultRequestUpdateRewards.opcode)
        # The anchor is a CallContractBlock, so the only jetton_transfer /
        # ton_transfer in the consumed set are the withdraw and the unstake fee.
        withdraw = _first_jetton_transfer(consumed)
        fee = _first_ton_transfer(consumed)
        return await build_jvault_unstake_core(anchor, request_update, withdraw, fee)

    @reg.register_builder("build_jvault_claim")
    async def build_jvault_claim(m):
        consumed = m._mch_consumed
        anchor = consumed[0]  # the claim call
        send_to_pool = _first_call(consumed, JVaultSendClaimedRewards.opcode)
        withdrawal = _first_jetton_transfer(consumed)
        return build_jvault_claim_core(anchor, send_to_pool, withdrawal)

    @reg.register_builder("build_jvault_unstake_request")
    async def build_jvault_unstake_request(m):
        consumed = m._mch_consumed
        anchor = consumed[0]  # the unstake_request call
        request_update = _first_call(consumed, JVaultRequestUpdateRewards.opcode)
        return await build_jvault_unstake_request_core(anchor, request_update)

    return reg


def jvault_registries() -> Registries:
    """Fresh Registries ready to compile specs/jvault.mch."""
    return register_jvault(Registries())
