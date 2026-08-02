"""Register the nominator pool matchers from specs/nominator.mch.

The withdrawal shape anchors each payout instead of fanning out from the
NominatorPoolProcessWithdrawRequests call. Produced fields use an `out` block.
Host hooks handle the parent topology check, payout arithmetic and shaping,
and the nullable NominatorPool interface lookup.
"""
from __future__ import annotations

from indexer.events import context
from indexer.events.blocks.basic_blocks import CallContractBlock, TonTransferBlock
from indexer.events.blocks.core import Block
from indexer.events.blocks.messages.staking import NominatorPoolProcessWithdrawRequests
from indexer.events.mch.registry import Registries

BLOCK_TYPES = {
    "nominator_pool_deposit": Block,
    "nominator_pool_withdraw_request": Block,
}


def _classified_withdraw_payouts(request: Block | None) -> list[TonTransferBlock]:
    """Return the payouts legacy consumes: all of them only at arity 1 or 2."""
    if request is None:
        return []
    payouts = [b for b in request.next_blocks if isinstance(b, TonTransferBlock)]
    payouts.sort(key=lambda b: b.min_lt)
    return payouts if len(payouts) in (1, 2) else []


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
        # Nullable interface lookup: resolves the payout's pool address to
        # its interface set, returning it only when the NominatorPool interface
        # is present (else None -> the build `reject when pool_iface == null`).
        repo = context.interface_repository.get()
        if repo is None:
            return None
        interfaces = await repo.get_interfaces(address)
        return interfaces if "NominatorPool" in interfaces else None

    @reg.register_fn("nominator_withdraw_payout_amount")
    async def nominator_withdraw_payout_amount(request: Block):
        payouts = _classified_withdraw_payouts(request)
        if len(payouts) == 2:
            return payouts[0].data["value"]
        if len(payouts) == 1 and payouts[0].value > request.get_message().value:
            return payouts[0].data["value"]
        return None

    @reg.register_shaper("nominator_withdraw_absorb_payouts")
    def nominator_withdraw_absorb_payouts(produced: Block, match) -> None:
        request = next(
            (b for b in match.consumed
             if isinstance(b, TonTransferBlock) and b.comment == "w"),
            None,
        )
        payouts = _classified_withdraw_payouts(request)
        if not payouts:
            return

        # The wrapper merge has made the payouts siblings of `request` under
        # produced. Give merge_blocks its required connected view temporarily;
        # then restore the same child links a one-shot legacy merge leaves.
        outer = produced.previous_block
        first_next = payouts[0].next_blocks.copy()
        payouts[0].previous_block = outer
        if len(payouts) == 2:
            payouts[0].next_blocks.append(payouts[1])
            payouts[1].previous_block = payouts[0]
        try:
            produced.merge_blocks(payouts)
        finally:
            payouts[0].next_blocks = first_next
            for payout in payouts:
                payout.previous_block = request
        produced.previous_block = outer
        produced.compact_connections()

    return reg


def nominator_registries() -> Registries:
    """Fresh Registries ready to compile specs/nominator.mch."""
    return register_nominator(Registries())
