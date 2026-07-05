"""mch registries for the tonstakers withdraw matcher (specs/tonstakers.mch).

HYBRID declarative build: the produced btype/fields/flags are expressed with a
`produces switch` + `out {...}` in the spec; only two genuinely inexpressible
pieces stay as host fns (each a named language gap documented in the spec):

  tonstakers_immediate_amount — the immediate payout is
    `immediate.msg.value - request.msg.value`, and the expression grammar has no
    binary arithmetic (only unary '-').
  tonstakers_minted_nft — reads the minted request NFT off
    delayed.next_blocks; block-topology navigation is excluded from the language
    (§15b), and the NFT is a NON-consumed neighbor (NftMintBlockMatcher's own
    sibling nft_mint), so it cannot be a captured pattern node without diverging
    the consumed set.

The declarative product is a generic Block + dict data (the canonical C++ shape).
The shared build_tonstakers_withdraw_core (blocks/staking.py) stays with the
legacy TONStakersWithdrawMatcher on the production path only.
"""
from __future__ import annotations

from indexer.events.blocks.core import Block
from indexer.events.blocks.messages.jettons import (
    JettonBurn,
    JettonBurnNotification,
)
from indexer.events.blocks.messages.staking import (
    TONStakersInitNFT,
    TONStakersMintNFT,
    TONStakersPoolWithdrawal,
    TONStakersWithdrawRequest,
)
from indexer.events.blocks.nft import NftMintBlock
from indexer.events.blocks.utils import AccountId, Amount
from indexer.events.blocks.utils.block_utils import find_call_contract
from indexer.events.mch.registry import Registries, default_message_types

OPCODES = {
    "JettonBurn":                JettonBurn.opcode,
    "JettonBurnNotification":    JettonBurnNotification.opcode,
    "TONStakersWithdrawRequest": TONStakersWithdrawRequest.opcode,
    "TONStakersPoolWithdrawal":  TONStakersPoolWithdrawal.opcode,
    "TONStakersMintNFT":         TONStakersMintNFT.opcode,
}

# btype strings the `produces switch` selects — a declarative build emits a
# generic Block with these btypes (not the TONStakers*Block subclasses, which
# have no place in the C++ engine).
BLOCK_TYPES = {
    "tonstakers_withdraw":         Block,
    "tonstakers_withdraw_request": Block,
}


async def _tonstakers_immediate_amount(immediate, request):
    """Immediate payout = pool-withdrawal msg.value minus withdraw-request
    msg.value (GAP: no binary arithmetic in the expression grammar)."""
    if immediate is None or request is None:
        return None
    return Amount(immediate.get_message().value - request.get_message().value)


async def _tonstakers_minted_nft(delayed):
    """Address of the minted request NFT, read off delayed.next_blocks (GAP:
    block-topology navigation, and the NFT stays a non-consumed neighbor).
    Returns None when absent (the delayed arm then marks the block failed)."""
    if delayed is None:
        return None
    nft = next((b for b in delayed.next_blocks if isinstance(b, NftMintBlock)), None)
    if nft is None:
        nft = find_call_contract(delayed.next_blocks, TONStakersInitNFT.opcode)
    if nft is None:
        return None
    return AccountId(nft.event_nodes[0].message.destination)


def register_tonstakers(reg: Registries) -> Registries:
    """Register everything specs/tonstakers.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)
    reg.message_types.update(default_message_types())
    reg.register_fn("tonstakers_immediate_amount")(_tonstakers_immediate_amount)
    reg.register_fn("tonstakers_minted_nft")(_tonstakers_minted_nft)
    return reg


def tonstakers_registries() -> Registries:
    """Fresh Registries ready to compile specs/tonstakers.mch."""
    return register_tonstakers(Registries())
