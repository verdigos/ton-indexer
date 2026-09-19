from __future__ import annotations

import pytest

from indexer.events import context
from indexer.events.mch.registry import (
    Registries,
    default_lookups,
    default_message_types,
)
from tests.utils.repository import TestInterfaceRepository


def test_register_predicate_via_method():
    reg = Registries()

    @reg.register_predicate("pton_self_transfer")
    def pton_self_transfer(block):  # noqa: ARG001
        return True

    assert reg.predicates["pton_self_transfer"] is pton_self_transfer


def test_double_predicate_registration_raises():
    reg = Registries()

    @reg.register_predicate("p")
    def first(b):  # noqa: ARG001
        return True

    with pytest.raises(KeyError, match="predicate 'p' already registered"):
        @reg.register_predicate("p")
        def second(b):  # noqa: ARG001
            return False


def test_register_opcode():
    reg = Registries()
    reg.opcodes["JettonTransfer"] = 0x0f8a7ea5
    assert reg.opcodes["JettonTransfer"] == 0x0f8a7ea5


def test_new_fields_empty_by_default():
    reg = Registries()
    assert reg.message_types == {}
    assert reg.lookups == {}


def test_register_lookup_via_method():
    reg = Registries()

    @reg.register_lookup("jetton_wallet")
    async def jetton_wallet(addr):  # noqa: ARG001
        return None

    assert reg.lookups["jetton_wallet"] is jetton_wallet


def test_double_lookup_registration_raises():
    reg = Registries()

    @reg.register_lookup("jetton_wallet")
    async def first(addr):  # noqa: ARG001
        return None

    with pytest.raises(KeyError, match="lookup 'jetton_wallet' already registered"):
        @reg.register_lookup("jetton_wallet")
        async def second(addr):  # noqa: ARG001
            return None


JETTON_FAMILY = {
    "JettonTransfer",
    "JettonInternalTransfer",
    "JettonNotify",
    "JettonBurn",
    "JettonBurnNotification",
    "JettonMint",
    "MinterJettonMint",
}


def test_default_message_types_covers_jetton_family():
    types = default_message_types()
    assert JETTON_FAMILY <= set(types), (
        f"missing: {JETTON_FAMILY - set(types)}"
    )
    # Keys are class names, values are those classes.
    for name, cls in types.items():
        assert cls.__name__ == name


def test_default_message_types_covers_referenced_swap_messages():
    types = default_message_types()
    # Swap opcodes referenced by dedust_swap.mch / stonfi_v2_swap.mch.
    assert {"DedustSwapExternal", "DedustPayoutFromPool", "PTonTransfer",
            "StonfiV2PayTo"} <= set(types)


def test_default_lookups_names():
    lookups = default_lookups()
    assert set(lookups) == {"jetton_wallet", "nft_item"}


@pytest.mark.asyncio
async def test_jetton_wallet_lookup_resolves_and_normalizes_address():
    wallet_addr = "0:" + "AB" * 32  # raw-form, uppercase
    interfaces = {
        wallet_addr: {
            "JettonWallet": {
                "balance": 1000,
                "address": wallet_addr,
                "owner": "0:" + "11" * 32,
                "jetton": "0:" + "22" * 32,
            }
        }
    }
    token = context.interface_repository.set(TestInterfaceRepository(interfaces))
    try:
        jetton_wallet = default_lookups()["jetton_wallet"]
        # Pass a lowercased address to prove .upper() normalization hits the
        # uppercase interface key.
        result = await jetton_wallet(wallet_addr.lower())
    finally:
        context.interface_repository.reset(token)

    assert result is not None
    assert result.address == wallet_addr
    assert result.owner == "0:" + "11" * 32
    assert result.jetton == "0:" + "22" * 32


@pytest.mark.asyncio
async def test_jetton_wallet_lookup_returns_none_when_absent():
    token = context.interface_repository.set(TestInterfaceRepository({}))
    try:
        result = await default_lookups()["jetton_wallet"]("0:" + "CD" * 32)
    finally:
        context.interface_repository.reset(token)
    assert result is None
