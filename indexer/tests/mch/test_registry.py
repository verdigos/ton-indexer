from __future__ import annotations

import pytest

from indexer.events.mch.registry import Registries


def test_register_predicate_via_method():
    reg = Registries()

    @reg.register_predicate("pton_self_transfer")
    def pton_self_transfer(block):  # noqa: ARG001
        return True

    assert reg.predicates["pton_self_transfer"] is pton_self_transfer


def test_register_builder_via_method():
    reg = Registries()

    @reg.register_builder("build_jetton_transfer")
    async def build_jetton_transfer(m):  # noqa: ARG001
        return None

    assert reg.builders["build_jetton_transfer"] is build_jetton_transfer


def test_double_predicate_registration_raises():
    reg = Registries()

    @reg.register_predicate("p")
    def first(b):  # noqa: ARG001
        return True

    with pytest.raises(KeyError, match="predicate 'p' already registered"):
        @reg.register_predicate("p")
        def second(b):  # noqa: ARG001
            return False


def test_double_builder_registration_raises():
    reg = Registries()

    @reg.register_builder("b")
    async def first(m):  # noqa: ARG001
        return None

    with pytest.raises(KeyError, match="builder 'b' already registered"):
        @reg.register_builder("b")
        async def second(m):  # noqa: ARG001
            return None


def test_register_opcode_and_block_type():
    reg = Registries()
    reg.opcodes["JettonTransfer"] = 0x0f8a7ea5
    reg.block_types["JettonTransferBlock"] = object  # placeholder
    assert reg.opcodes["JettonTransfer"] == 0x0f8a7ea5
