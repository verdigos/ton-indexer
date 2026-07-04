"""mch builder for the tgbtc_mint matcher (specs/tgbtc.mch).

Thin adapter over the shared core `build_tgbtc_mint_core` in blocks/tgbtc.py —
the same function the legacy TgBTCMintBlockMatcher calls.

Consumed-set-driven (`Match._mch_consumed`), NOT capture-driven — same situation
as specs/evaa.mch: the three blocks the core needs sit in the recursion's exit
branch (@tail/@log/@mint would be list bindings under §7.2 and the compiled
path's positional walker mis-binds same-position exit captures). Each role
carries a unique signature within one match's consumed set, so the derivation is
unambiguous:

- head:        the sole CallContractBlock with opcode 0x3F781D24 (also the
               anchor, i.e. consumed[0]).
- success log: the sole CallContractBlock with opcode 0x77A80EF3.
- jetton_mint: the sole block with btype "jetton_mint".

Deployment side-effects: every CallContractBlock whose transaction deploys its
destination carries a `ContractDeploy` block in its `children_blocks` (attached
at init, sharing the parent's event node — NOT reachable via `next_blocks`, so
the structural match never consumes it). The legacy matcher sweeps these up in
its ancestor walk (`children_of_prev`) and merges them into the produced block;
to stay byte-identical (same event-node multiset, ContractDeploy as a direct
child) this builder collects them from the consumed set's `children_blocks` and
merges them before returning. The engine's own merge of the matched chain runs
afterward and composes cleanly (verified against legacy on all four fixtures).
The blocks are still surfaced as their own `contract_deploy` actions by the
`unwind_deployments` post-process pass, exactly as in the legacy path.
"""
from __future__ import annotations

from indexer.events.blocks.basic_blocks import CallContractBlock, ContractDeploy
from indexer.events.blocks.core import Block
from indexer.events.blocks.jettons import JettonMintBlock
from indexer.events.blocks.tgbtc import (
    TgBTCMintBlock,
    build_tgbtc_mint_core,
)
from indexer.events.blocks.messages.tgbtc import TgBTCMintEvent
from indexer.events.mch.registry import Registries

_HEAD_OPCODE = 0x3F781D24
_TAIL_OPCODE = 0xE42A879B

OPCODES = {
    "TgBTCMintHead":       _HEAD_OPCODE,
    "TgBTCMintTail":       _TAIL_OPCODE,
    "TgBTCMintSuccessLog": TgBTCMintEvent.opcode,
}

BLOCK_TYPES = {
    "TgBTCMintBlock": TgBTCMintBlock,
    "jetton_mint":    JettonMintBlock,
}


def _first_call(consumed: list[Block], opcode: int) -> Block | None:
    return next(
        (b for b in consumed
         if isinstance(b, CallContractBlock) and b.opcode == opcode),
        None,
    )


def register_tgbtc(reg: Registries) -> Registries:
    """Register everything specs/tgbtc.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_builder("build_tgbtc_mint")
    async def build_tgbtc_mint(m):
        consumed = m._mch_consumed
        head = _first_call(consumed, _HEAD_OPCODE)
        success_log = _first_call(consumed, TgBTCMintEvent.opcode)
        jetton_mint = next(
            (b for b in consumed if b.btype == "jetton_mint"), None
        )
        produced = build_tgbtc_mint_core(head, success_log, jetton_mint)
        if produced is None:
            return None
        deploys = [
            c
            for b in consumed
            for c in b.children_blocks
            if isinstance(c, ContractDeploy)
        ]
        if deploys:
            produced.merge_blocks(deploys)
        return produced

    return reg


def tgbtc_registries() -> Registries:
    """Fresh Registries ready to compile specs/tgbtc.mch."""
    return register_tgbtc(Registries())
