"""Shared `produces`-name normalization.

`produces` in .mch names either a host block CLASS or a bare btype string
(spec §8.1). IR emission and the declarative build path both need the btype
string, so the class -> btype map lives here. POC: static map; M5 resolves via
the real block-type registry (class -> instance btype) at emit time.
"""
from __future__ import annotations

CLASS_TO_BTYPE: dict[str, str] = {
    "JettonTransferBlock": "jetton_transfer",
    "JettonBurnBlock": "jetton_burn",
    "JettonMintBlock": "jetton_mint",
    "JettonSwapBlock": "jetton_swap",
    "LayerZeroReceiveBlock": "layerzero_receive",
    "EvaaWithdrawBlock": "evaa_withdraw",
    "TONStakersWithdrawBlock": "tonstakers_withdraw",
    "TONStakersWithdrawRequestBlock": "tonstakers_withdraw_request",
    "EthenaDepositBlock": "ethena_deposit",
    "VestingSendMessageBlock": "vesting_send_message",
    "ToncoDepositLiquidityBlock": "tonco_deposit_liquidity",
    "CoffeeCreateVaultBlock": "coffee_create_vault",
    "DedustDepositLiquidity": "dedust_deposit_liquidity",
    "DedustDepositLiquidityPartial": "dedust_deposit_liquidity_partial",
}


def produces_btype(name: str) -> str:
    return CLASS_TO_BTYPE.get(name, name)
