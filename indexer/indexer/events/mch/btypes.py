"""Shared `produces`-name normalization.

`produces` in .mch names either a host block CLASS (PascalCase, e.g.
`JettonTransferBlock`) or a bare btype string (snake_case, e.g. `jetton_transfer`)
— spec §8.1, §3.1. IR emission and the declarative build path both need the
btype STRING the produced block actually carries, so the class -> btype map
lives here.

`produces_btype` is a pure name lookup with an identity fallback: a snake_case
name IS its btype, and a PascalCase class name resolves through `CLASS_TO_BTYPE`.
It stays lenient (never raises) so emission is total even on synthetic test
matchers whose produced btype legitimately equals the name.

The F3 defect this guards against: a DECLARATIVE matcher whose `produces` names a
PascalCase block class absent from `CLASS_TO_BTYPE` would fall through to the
class-name STRING as the produced btype — and for a declarative matcher that
btype is what the engine stamps on the produced block (`Block(produces[0], …)`),
so the serializer and legacy blocks, which key on the real snake_case btype,
silently mismatch. The resolver (`R022_UNNORMALIZED_PRODUCES_CLASS`,
`_check_produces_union`) catches exactly that case loudly. It is scoped to
declarative matchers: a matcher with a host `build` fn constructs its own block,
so its `produces` btype is documentation only and an identity fallback is
harmless there.

Why a curated table and not registry resolution (the report's ideal): emission
runs on stub registries (`ir_emit.build_stub_registries`, block_types ->
`object`) to stay off the heavy block/DB import stack, so the produced class's
real instance btype is not available at emit time. Until emit gains real
block-type access (M5), the class -> btype mapping is curated here. Only
PascalCase class names used by declarative matchers need an entry; snake_case
producers and builder-matcher producers bypass the table.
"""
from __future__ import annotations

CLASS_TO_BTYPE: dict[str, str] = {
    "JettonTransferBlock": "jetton_transfer",
    "JettonBurnBlock": "jetton_burn",
    "JettonMintBlock": "jetton_mint",
    "JettonSwapBlock": "jetton_swap",
    "LayerZeroReceiveBlock": "layerzero_receive",
    "LayerZeroSendBlock": "layerzero_send",
    "LayerZeroSendTokensBlock": "layerzero_send_tokens",
    "LayerZeroCommitPacketBlock": "layerzero_commit_packet",
    "LayerZeroDvnVerifyBlock": "layerzero_dvn_verify",
    "EvaaSupplyBlock": "evaa_supply",
    "EvaaWithdrawBlock": "evaa_withdraw",
    "TONStakersWithdrawBlock": "tonstakers_withdraw",
    "TONStakersWithdrawRequestBlock": "tonstakers_withdraw_request",
    "EthenaDepositBlock": "ethena_deposit",
    "VestingSendMessageBlock": "vesting_send_message",
    "ToncoDepositLiquidityBlock": "tonco_deposit_liquidity",
    "CoffeeCreateVaultBlock": "coffee_create_vault",
    "DedustDepositLiquidity": "dedust_deposit_liquidity",
    "DedustDepositLiquidityPartial": "dedust_deposit_liquidity_partial",
    "NftPurchaseBlock": "nft_purchase",
    "NftTransferBlock": "nft_transfer",
    "TgBTCMintBlock": "tgbtc_mint",
}


def is_class_name(name: str) -> bool:
    """A `produces` token is a class name iff it is PascalCase (§3.1). btype
    names are snake_case and pass through `produces_btype` unchanged."""
    return bool(name) and name[0].isupper()


def produces_btype(name: str) -> str:
    return CLASS_TO_BTYPE.get(name, name)
