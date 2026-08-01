"""mch registries for the tonstakers family (specs/tonstakers.mch): the deposit,
the withdraw fork, and the delayed withdrawal's NFT-burn payout.

HYBRID declarative build: the produced btype/fields/flags are expressed with
`out {...}` / `produces switch` in the spec; the immediate payout is declarative
arithmetic (`amount(immediate.msg.value - request.msg.value)`, G-ARITH), leaving
TWO genuinely inexpressible pieces as host fns:

  tonstakers_minted_nft — reads the minted request NFT off
    delayed.next_blocks; block-topology navigation is excluded from the language
    (§15b), and the NFT is a NON-consumed neighbor (NftMintBlockMatcher's own
    sibling nft_mint), so it cannot be a captured pattern node without diverging
    the consumed set.

  tonstakers_pool_addr — walks `previous_block` upward an unbounded number of
    hops for the pool address (§15b again). A thin wrapper over the shared
    blocks/staking.py::find_tonstakers_pool_addr, which legacy calls too, so the
    two engines cannot drift.

The declarative product is a generic Block + dict data (the canonical C++ shape).
The shared build_tonstakers_withdraw_core (blocks/staking.py) stays with the
legacy TONStakersWithdrawMatcher on the production path only.
"""
from __future__ import annotations

from indexer.events.blocks.core import Block
from indexer.events.blocks.messages.jettons import (
    JettonBurn,
    JettonBurnNotification,
    JettonInternalTransfer,
    JettonNotify,
)
from indexer.events.blocks.messages.staking import (
    TONStakersDepositRequest,
    TONStakersDistributedAsset,
    TONStakersInitNFT,
    TONStakersMintJettons,
    TONStakersMintNFT,
    TONStakersNftBurn,
    TONStakersNftBurnNotification,
    TONStakersPoolWithdrawal,
    TONStakersWithdrawRequest,
)
from indexer.events.blocks.staking import find_tonstakers_pool_addr
from indexer.events.blocks.utils import AccountId
from indexer.events.blocks.utils.block_utils import find_call_contract
from indexer.events.mch.registry import Registries, default_message_types

OPCODES = {
    "JettonBurn":                    JettonBurn.opcode,
    "JettonBurnNotification":        JettonBurnNotification.opcode,
    "JettonInternalTransfer":        JettonInternalTransfer.opcode,
    "JettonNotify":                  JettonNotify.opcode,
    "TONStakersDepositRequest":      TONStakersDepositRequest.opcode,
    "TONStakersWithdrawRequest":     TONStakersWithdrawRequest.opcode,
    "TONStakersPoolWithdrawal":      TONStakersPoolWithdrawal.opcode,
    "TONStakersMintNFT":             TONStakersMintNFT.opcode,
    "TONStakersMintJettons":         TONStakersMintJettons.opcode,
    "TONStakersNftBurn":             TONStakersNftBurn.opcode,
    "TONStakersNftBurnNotification": TONStakersNftBurnNotification.opcode,
    "TONStakersDistributedAsset":    TONStakersDistributedAsset.opcode,
}

# btype strings the matchers produce — a declarative build emits a generic Block
# with these btypes (not the TONStakers*Block subclasses, which have no place in
# the C++ engine).
BLOCK_TYPES = {
    "tonstakers_deposit":          Block,
    "tonstakers_withdraw":         Block,
    "tonstakers_withdraw_request": Block,
}

# TONStakersNftBurnNotification is the family's only message with a real body
# (query_id / amount / owner); the C++ twin is the protocol ABI entry of the
# same name. The rest are opcode-only markers and are never `parse`d.
MESSAGE_TYPES = {
    "TONStakersNftBurnNotification": TONStakersNftBurnNotification,
}


async def _tonstakers_minted_nft(delayed):
    """Address of the minted request NFT, read off delayed.next_blocks (GAP:
    block-topology navigation, and the NFT stays a non-consumed neighbor).
    Returns None when absent (the delayed arm then marks the block failed)."""
    if delayed is None:
        return None
    # btype, not isinstance(NftMintBlock): specs/nft_mint.mch is declarative, so
    # the minted NFT is a generic Block under the mch engines. This arm is the
    # LIVE one in production (nft_mint has priority 80 and consumes the
    # TONStakersInitNFT call into itself, so the fallback below finds nothing);
    # the sweep's legacy NftMintBlockMatcher prereq hid that for one wave.
    nft = next((b for b in delayed.next_blocks if b.btype == 'nft_mint'), None)
    if nft is None:
        nft = find_call_contract(delayed.next_blocks, TONStakersInitNFT.opcode)
    if nft is None:
        return None
    return AccountId(nft.event_nodes[0].message.destination)


async def _tonstakers_pool_addr(notification):
    """Pool address of a delayed withdrawal (GAP: unbounded previous_block walk).
    Delegates to the shared legacy core so both engines run the same walk."""
    if notification is None:
        return None
    return find_tonstakers_pool_addr(notification)


def register_tonstakers(reg: Registries) -> Registries:
    """Register everything specs/tonstakers.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)
    reg.message_types.update(default_message_types())
    reg.message_types.update(MESSAGE_TYPES)
    reg.register_fn("tonstakers_minted_nft")(_tonstakers_minted_nft)
    reg.register_fn("tonstakers_pool_addr")(_tonstakers_pool_addr)
    return reg


def tonstakers_registries() -> Registries:
    """Fresh Registries ready to compile specs/tonstakers.mch."""
    return register_tonstakers(Registries())
