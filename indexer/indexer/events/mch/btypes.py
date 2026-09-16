"""Leaf btypes the C++ engine creates without a matcher.

`LEAF_BTYPES` mirrors `kLeafBtypes` in ton-index-worker/ton-mch-engine/src/IrTables.h.
"""
from __future__ import annotations

LEAF_BTYPES: frozenset[str] = frozenset({
    "call_contract", "contract_deploy", "empty", "gasless_request",
    "jetton_transfer", "root", "tick_tock", "ton_transfer",
})
