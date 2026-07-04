"""mch builders for the dedust matchers (specs/dedust_swap.mch,
specs/dedust_withdraw.mch, specs_decl/dedust_swap_hybrid.mch).

Swap: the legacy build (blocks/swaps.py build_dedust_swap_core) scans the full
consumed block set — find_messages/find_call_contracts over other_blocks plus
`in include` membership checks — which captures cannot reconstruct (blocks
matched by the recursion's exit branch have no bindings). The builder therefore
uses the well-known `Match._mch_consumed` attribute (anchor first) and
delegates to the shared core. Returns None to reject the match (spec §10.7).

Withdraw: no recursion in the pattern, all captures bind positionally — the
builder is fully capture-driven (anchor + the two payout terminals).

Hybrid swap (specs_decl): the declarative build owns the reject/failed wiring
and the scalar out fields it can genuinely derive; the two host fns below own
the imperative residue (peer-swap list assembly, payout resolution, BOC
steps/pool-hop walk, notification-coverage failure scan), mirroring the legacy
core line-for-line so parity holds by construction.
"""
from __future__ import annotations

from types import SimpleNamespace

from pytoniq_core import Slice

from indexer.events.blocks.basic_blocks import CallContractBlock
from indexer.events.blocks.core import Block
from indexer.events.blocks.jettons import JettonTransferBlock
from indexer.events.blocks.liquidity import build_dedust_withdraw_core
from indexer.events.blocks.messages import (
    DedustPayout,
    DedustPayoutFromPool,
    DedustSwap,
    DedustSwapExternal,
    DedustSwapNotification,
    DedustSwapPeer,
)
from indexer.events.blocks.messages.jettons import (
    JettonBurn,
    JettonBurnNotification,
)
from indexer.events.blocks.messages.swaps import DedustSwapPayload
from indexer.events.blocks.swaps import (
    JettonSwapBlock,
    _get_dedust_jetton_swap_sequence,
    _parse_dedust_steps,
    _unwrap_dedust_include,
    build_dedust_swap_core,
)
from indexer.events.blocks.utils import AccountId, Amount, Asset
from indexer.events.blocks.utils.block_utils import (
    find_call_contracts,
    find_messages,
)
from indexer.events.mch.registry import Registries

OPCODES = {
    "DedustSwapExternal":     DedustSwapExternal.opcode,
    "DedustSwapPeer":         DedustSwapPeer.opcode,
    "DedustSwap":             DedustSwap.opcode,
    "DedustSwapNotification": DedustSwapNotification.opcode,
    "DedustPayoutFromPool":   DedustPayoutFromPool.opcode,
    "DedustPayout":           DedustPayout.opcode,
}

BLOCK_TYPES = {
    "JettonSwapBlock": JettonSwapBlock,
}

WITHDRAW_OPCODES = {
    "JettonBurn":             JettonBurn.opcode,
    "JettonBurnNotification": JettonBurnNotification.opcode,
    "DedustPayoutFromPool":   DedustPayoutFromPool.opcode,
    "DedustPayout":           DedustPayout.opcode,
}

WITHDRAW_BLOCK_TYPES = {
    "dex_withdraw_liquidity": Block,
}


def register_dedust(reg: Registries) -> Registries:
    """Register everything specs/dedust_swap.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_builder("build_dedust_swap")
    async def build_dedust_swap(m):
        consumed = m._mch_consumed
        return await build_dedust_swap_core(consumed[0], consumed[1:])

    return reg


def dedust_registries() -> Registries:
    """Fresh Registries ready to compile specs/dedust_swap.mch."""
    return register_dedust(Registries())


async def _dedust_swap_legs(consumed: list[Block],
                            in_transfer: Block | None,
                            swap_request: Block | None) -> SimpleNamespace | None:
    """Imperative residue of the dedust swap build, restructured from
    blocks/swaps.py build_dedust_swap_core (kept line-for-line where possible):
    peer-swap assembly, in-leg extraction, payout resolution, and the BOC
    steps/pool-hop walk. Returns None to reject (wrong payload opcode), raises
    to reject on malformed traces/missing pool data — both per spec §10.7.

    The branch discriminator differs from legacy in form, not substance: legacy
    checks `block.previous_block.btype == 'jetton_transfer' and ... in
    other_blocks` / scans for a DedustSwap request; the pattern's OR-parent
    captures bind exactly one of in_transfer/swap_request to the same blocks.
    """
    anchor = consumed[0]
    other_blocks = list(consumed[1:])
    include = _unwrap_dedust_include(anchor, other_blocks)

    # Successful hops, one per DedustSwapNotification, in lt order.
    messages = find_messages(include, DedustSwapNotification)
    messages.sort(key=lambda x: x[0].min_lt)
    peer_swaps = []
    for _, message in messages:
        peer_swaps.append({
            'in': {
                'amount': Amount(message.amount_in),
                'asset': Asset(is_ton=message.asset_in.is_ton,
                               jetton_address=message.asset_in.jetton_address),
            },
            'out': {
                'amount': Amount(message.amount_out),
                'asset': Asset(is_ton=message.asset_out.is_ton,
                               jetton_address=message.asset_out.jetton_address),
            }
        })

    # In leg.
    sender = None
    sender_wallet = None
    dex_incoming_jetton_wallet = None
    dex_incoming_wallet = None
    amount_in = None
    asset_in = None
    swap_steps_slice = None
    if in_transfer is not None:
        dex_incoming_jetton_wallet = in_transfer.data['receiver_wallet']
        dex_incoming_wallet = in_transfer.data['receiver']
        sender_wallet = in_transfer.data['sender_wallet']
        sender = in_transfer.data['sender']
        asset_in = in_transfer.data['asset']
        if int(in_transfer.data['payload_opcode'], 0) != DedustSwapPayload.opcode:
            return None
        amount_in = in_transfer.data['amount']
        swap_steps_slice = Slice.one_from_boc(in_transfer.data['forward_payload'])
        swap_steps_slice.skip_bits(32)  # sum type
    elif swap_request is not None:
        sender = AccountId(swap_request.get_message().source)
        dex_incoming_wallet = AccountId(swap_request.get_message().destination)
        s = swap_request.get_body()
        s.load_uint(32 + 64)
        amount_in = s.load_coins()
        swap_steps_slice = s
        asset_in = Asset(is_ton=True)

    # Payout resolution.
    payout_from_pool = find_call_contracts(other_blocks, DedustPayoutFromPool.opcode)
    assert len(payout_from_pool) == 1, "Expected one payout from pool"
    receiver = sender
    receiver_wallet = None
    dex_outgoing_jetton_wallet = None
    dex_outgoing_wallet = None
    actual_amount_out = DedustPayoutFromPool(payout_from_pool[0].get_body()).amount
    actual_asset_out = None
    for payout_block in payout_from_pool[0].next_blocks:
        if payout_block in include:
            if isinstance(payout_block, JettonTransferBlock):
                receiver_wallet = payout_block.data['receiver_wallet']
                receiver = payout_block.data['receiver']
                dex_outgoing_wallet = payout_block.data['sender']
                dex_outgoing_jetton_wallet = payout_block.data['sender_wallet']
                actual_asset_out = payout_block.data['asset']
                actual_amount_out = payout_block.data['amount']
            elif isinstance(payout_block, CallContractBlock) and payout_block.opcode == DedustPayout.opcode:
                dex_outgoing_wallet = AccountId(payout_block.get_message().source)
                receiver = AccountId(payout_block.get_message().destination)
                actual_asset_out = Asset(is_ton=True)

    # Desired out asset: BOC steps walk + pool-hop lookups.
    steps = _parse_dedust_steps(swap_steps_slice)
    swap_sequence = await _get_dedust_jetton_swap_sequence(
        steps, Asset(is_ton=True) if asset_in is None else asset_in)
    destination_asset = swap_sequence[-1]

    # No successful hop at all: synthesize the single leg from actual values.
    if len(peer_swaps) == 0:
        peer_swaps.append({
            'in': {
                'amount': Amount(amount_in),
                'asset': asset_in
            },
            'out': {
                'amount': Amount(actual_amount_out),
                'asset': actual_asset_out
            }
        })

    return SimpleNamespace(
        sender=sender,
        asset_in=asset_in,
        destination_asset=destination_asset,
        dex_incoming_transfer={
            'asset': peer_swaps[0]['in']['asset'],
            'amount': peer_swaps[0]['in']['amount'],
            'source': sender,
            'source_jetton_wallet': sender_wallet,
            'destination': dex_incoming_wallet,
            'destination_jetton_wallet': dex_incoming_jetton_wallet,
        },
        dex_outgoing_transfer={
            'asset': peer_swaps[-1]['out']['asset'],
            'amount': peer_swaps[-1]['out']['amount'],
            'source': dex_outgoing_wallet,
            'source_jetton_wallet': dex_outgoing_jetton_wallet,
            'destination': receiver,
            'destination_jetton_wallet': receiver_wallet,
        },
        in_leg=peer_swaps[0]['in'],
        out_leg=peer_swaps[-1]['out'],
        peer_swaps=peer_swaps if len(peer_swaps) > 1 else [],
    )


async def _dedust_swap_failed(consumed: list[Block]) -> bool:
    """True when some swap request in the consumed set lacks a corresponding
    DedustSwapNotification (legacy failure scan, verbatim)."""
    include = _unwrap_dedust_include(consumed[0], list(consumed[1:]))
    for b in consumed:
        if isinstance(b, CallContractBlock) and b.opcode in (DedustSwapExternal.opcode,
                                                             DedustSwapPeer.opcode):
            has_swap_notification = False
            for n in b.next_blocks:
                if n in include and isinstance(n, CallContractBlock) and n.opcode == DedustSwapNotification.opcode:
                    has_swap_notification = True
                    break
            if not has_swap_notification:
                return True
    return False


def register_dedust_hybrid(reg: Registries) -> Registries:
    """Register everything specs_decl/dedust_swap_hybrid.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)
    reg.register_fn("dedust_swap_legs")(_dedust_swap_legs)
    reg.register_fn("dedust_swap_failed")(_dedust_swap_failed)
    return reg


def dedust_hybrid_registries() -> Registries:
    """Fresh Registries ready to compile specs_decl/dedust_swap_hybrid.mch."""
    return register_dedust_hybrid(Registries())


def register_dedust_withdraw(reg: Registries) -> Registries:
    """Register everything specs/dedust_withdraw.mch needs into `reg`."""
    reg.opcodes.update(WITHDRAW_OPCODES)
    reg.block_types.update(WITHDRAW_BLOCK_TYPES)

    @reg.register_builder("build_dedust_withdraw")
    async def build_dedust_withdraw(m):
        return await build_dedust_withdraw_core(m.burn, [m.payout_1, m.payout_2])

    return reg


def dedust_withdraw_registries() -> Registries:
    """Fresh Registries ready to compile specs/dedust_withdraw.mch."""
    return register_dedust_withdraw(Registries())
