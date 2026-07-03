"""Host-side builder registrations for `.mch` matchers.

Each protocol module exposes `register_*` (populate an existing Registries)
and a `*_registries` factory (fresh Registries ready to compile the matching
spec file in `indexer/events/mch/specs/`).
"""
from __future__ import annotations

from indexer.events.mch.builders.coffee import coffee_registries, register_coffee
from indexer.events.mch.builders.dedust import (
    dedust_registries,
    dedust_withdraw_registries,
    register_dedust,
    register_dedust_withdraw,
)
from indexer.events.mch.builders.dns import dns_registries, register_dns
from indexer.events.mch.builders.jettons import jetton_registries, register_jettons

__all__ = [
    "coffee_registries",
    "dedust_registries",
    "dedust_withdraw_registries",
    "dns_registries",
    "jetton_registries",
    "register_coffee",
    "register_dedust",
    "register_dedust_withdraw",
    "register_dns",
    "register_jettons",
]
