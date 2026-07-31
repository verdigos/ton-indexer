"""Registries for the fully declarative subscription matchers."""
from __future__ import annotations

from indexer.events.blocks.core import Block
from indexer.events.blocks.messages.subscriptions import (
    SubscriptionPayment,
    SubscriptionPaymentRequest,
    SubscriptionPaymentRequestResponse,
    WalletPluginDestruct,
)
from indexer.events.mch.registry import Registries

OPCODES = {
    "Request": SubscriptionPaymentRequest.opcode,
    "Response": SubscriptionPaymentRequestResponse.opcode,
    "Payment": SubscriptionPayment.opcode,
    "WalletPluginDestruct": WalletPluginDestruct.opcode,
}
BLOCK_TYPES = {"subscribe": Block, "unsubscribe": Block}
MESSAGE_TYPES = {"SubscriptionPaymentRequest": SubscriptionPaymentRequest}


def register_subscriptions(reg: Registries) -> Registries:
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)
    reg.message_types.update(MESSAGE_TYPES)
    return reg


def subscriptions_registries() -> Registries:
    return register_subscriptions(Registries())
