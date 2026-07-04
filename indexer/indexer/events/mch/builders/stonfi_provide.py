"""mch builders for the stonfi_v2 provide-liquidity matchers
(specs/stonfi_v2_provide.mch).

R08 remake of StonfiV2ProvideLiquidityMatcher. Each matcher builds ONE
deposit leg and returns a class-tagged block (StonfiV2DepositLiquidity /
StonfiV2DepositLiquidityPartial); cross-branch pairing, the EmptyBlock proxy
surgery and the legacy condition-fail absorption all live in the host pass
post_process_stonfi_v2_liquidity (blocks/liquidity.py, spec §5.5).

Builders are consumed-set driven (Match._mch_consumed, anchor first) and
delegate the data build to the shared build_stonfi_v2_provide_core — the same
fn the legacy matcher now calls. The partial builder carries the legacy
downgrade guard: when the anchor's add-liquidity child has a full callback
subtree (cb -> LP mint | refund), the final matcher owned the leg and its
builder rejection must reproduce the legacy whole-matcher rejection, so the
partial rejects too instead of matching the leg without the callback.
"""
from __future__ import annotations

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block
from indexer.events.blocks.liquidity import (
    StonfiV2DepositLiquidity,
    StonfiV2DepositLiquidityPartial,
    build_stonfi_v2_provide_core,
)
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
    "StonfiV2DepositLiquidity":        StonfiV2DepositLiquidity,
    "StonfiV2DepositLiquidityPartial": StonfiV2DepositLiquidityPartial,
}


def _is_call(block, opcode: int) -> bool:
    return isinstance(block, CallContractBlock) and block.opcode == opcode


def _resolve_in_transfer(anchor: Block) -> Block | None:
    """The block the legacy matcher labels 'in_transfer': the prebuilt
    jetton_transfer block, or the raw pTON 0x01f3835d call for a TON leg."""
    prev = anchor.previous_block
    if prev is None:
        return None
    if prev.btype == 'jetton_transfer':
        return prev
    if _is_call(prev, OPCODES["JettonNotify"]):
        p = prev.previous_block
        if _is_call(p, OPCODES["PTonTransfer"]):
            return p
        if _is_call(p, OPCODES["JettonInternalTransfer"]):
            return p.previous_block
    return None


def register_stonfi_provide(reg: Registries) -> Registries:
    """Register everything specs/stonfi_v2_provide.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_builder("build_stonfi_v2_provide_final")
    async def build_stonfi_v2_provide_final(m):
        consumed = m._mch_consumed
        anchor = consumed[0]
        try:
            in_transfer = _resolve_in_transfer(anchor)
            if in_transfer is None:
                return None
            add = next(b for b in consumed
                       if _is_call(b, OPCODES["StonfiV2AddLiquidity"])
                       and b.previous_block is anchor)
            cb = next(b for b in consumed
                      if _is_call(b, OPCODES["StonfiV2CbAddLiquidity"])
                      and b.previous_block is add)
            lp_transfer = next((b for b in consumed
                                if _is_call(b, OPCODES["JettonInternalTransfer"])
                                and b.previous_block is cb), None)
            refund = next((b for b in consumed
                           if _is_call(b, OPCODES["StonfiV2AddLiquidity"])
                           and b.previous_block is cb), None)
            data = build_stonfi_v2_provide_core(anchor, in_transfer, lp_transfer)
        except Exception:
            return None
        new_block = StonfiV2DepositLiquidity(data)
        new_block.failed = refund is not None
        return new_block

    @reg.register_builder("build_stonfi_v2_provide_partial")
    async def build_stonfi_v2_provide_partial(m):
        consumed = m._mch_consumed
        anchor = consumed[0]
        try:
            add = next(b for b in consumed
                       if _is_call(b, OPCODES["StonfiV2AddLiquidity"])
                       and b.previous_block is anchor)
            for cb in add.next_blocks:
                if _is_call(cb, OPCODES["StonfiV2CbAddLiquidity"]):
                    for t in cb.next_blocks:
                        if _is_call(t, OPCODES["JettonInternalTransfer"]) or \
                                _is_call(t, OPCODES["StonfiV2AddLiquidity"]):
                            # Full callback subtree: the final matcher owned
                            # this leg; its rejection == legacy rejection.
                            return None
            in_transfer = _resolve_in_transfer(anchor)
            if in_transfer is None:
                return None
            data = build_stonfi_v2_provide_core(anchor, in_transfer, None)
        except Exception:
            return None
        return StonfiV2DepositLiquidityPartial(data)

    return reg


def stonfi_provide_registries() -> Registries:
    """Fresh Registries ready to compile specs/stonfi_v2_provide.mch."""
    return register_stonfi_provide(Registries())
