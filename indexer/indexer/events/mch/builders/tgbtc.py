"""mch host fn + shaper for the tgbtc_mint matcher (specs/tgbtc.mch).

HYBRID declarative build (C++-runnable): the matcher has no `build` directive —
the produced btype, the reject decision and the out-field mapping live in the
.mch; the imperative residue is the single host fn `tgbtc_mint_data` below. It
delegates to the shared core `build_tgbtc_mint_core` in blocks/tgbtc.py (the same
function the legacy matcher calls) and exposes the produced TgBTCMintData
dataclass as a namespace. `null` from the core (missing pieces / unparseable log)
drives `reject when d == null`.

Consumed-set-driven (the well-known `consumed` env name), NOT capture-driven —
same situation as specs/evaa.mch: the three blocks the core needs sit in the
recursion's exit branch (@tail/@log/@mint would be list bindings under §7.2 and
the compiled path's positional walker mis-binds same-position exit captures).
Each role carries a unique signature within one match's consumed set:

- head:        the sole CallContractBlock with opcode 0x3F781D24 (also the anchor).
- success log: the sole CallContractBlock with opcode 0x77A80EF3.
- jetton_mint: the sole block with btype "jetton_mint".

Deployment side-effects ride on the shaper `tgbtc_deploy_absorb`: every
CallContractBlock whose transaction deploys its destination carries a
`ContractDeploy` block in its `children_blocks` (attached at init, sharing the
parent's event node — NOT reachable via `next_blocks`, so the structural match
never consumes it). The legacy matcher sweeps these up and merges them into the
produced block; to stay byte-identical (ContractDeploy as a direct child) the
shaper collects them from the consumed set's `children_blocks` and merges them as
the last build step, after the wrapper's consumed-merge. `unwind_deployments`
still surfaces them as their own `contract_deploy` actions.
"""
from __future__ import annotations

from dataclasses import fields as dc_fields
from types import SimpleNamespace

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

    @reg.register_fn("tgbtc_mint_data")
    async def tgbtc_mint_data(consumed):
        head = _first_call(consumed, _HEAD_OPCODE)
        success_log = _first_call(consumed, TgBTCMintEvent.opcode)
        jetton_mint = next(
            (b for b in consumed if b.btype == "jetton_mint"), None
        )
        produced = build_tgbtc_mint_core(head, success_log, jetton_mint)
        if produced is None:
            return None
        return SimpleNamespace(
            **{f.name: getattr(produced.data, f.name) for f in dc_fields(produced.data)}
        )

    @reg.register_shaper("tgbtc_deploy_absorb")
    def tgbtc_deploy_absorb(produced: Block, match) -> None:
        deploys = [
            c
            for b in match.consumed
            for c in b.children_blocks
            if isinstance(c, ContractDeploy)
        ]
        if deploys:
            produced.merge_blocks(deploys)

    return reg


def tgbtc_registries() -> Registries:
    """Fresh Registries ready to compile specs/tgbtc.mch."""
    return register_tgbtc(Registries())
