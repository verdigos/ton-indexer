"""Single source of truth for the combined host-builder registries the A/B twins run.

``py_match_dump`` / ``py_classify`` / ``py_actions`` each used to carry a
byte-identical copy of the builder import block, the ``_FACTORIES`` list, the
``_DICTS`` tuple and ``combined_registries()`` — three places to edit when a
protocol is added, and three places to forget one (same problem ``cpp_surface.py``
solved for the C++ registry surface).

Adding a protocol: import its ``*_registries`` factory below and append it to
``_FACTORIES``. That is the ONLY twin-side edit; the three twins pick it up.

The merge is last-wins per dict key in ``_FACTORIES`` order, which is why the
order is preserved verbatim from the twins it replaces.
"""
from __future__ import annotations

import sys

sys.path.insert(0, r"S:\Dev\ton-indexer-clear\indexer")

from indexer.events.mch.registry import Registries  # noqa: E402

from indexer.events.mch.builders import (  # noqa: E402
    coffee_registries, dedust_hybrid_registries, dedust_registries,
    dedust_withdraw_registries, dedust_v2_claim_reward_registries,
    dns_registries, jetton_decl_registries,
    jetton_registries, stonfi_v2_registries,
)
from indexer.events.mch.builders.coffee import coffee_liquidity_registries  # noqa: E402
from indexer.events.mch.builders.dedust import dedust_deposit_registries  # noqa: E402
from indexer.events.mch.builders.ethena import ethena_registries  # noqa: E402
from indexer.events.mch.builders.evaa import evaa_registries  # noqa: E402
from indexer.events.mch.builders.getgems import getgems_registries  # noqa: E402
from indexer.events.mch.builders.jvault import jvault_registries  # noqa: E402
from indexer.events.mch.builders.layerzero import layerzero_receive_registries  # noqa: E402
from indexer.events.mch.builders.nft_transfer import nft_transfer_registries  # noqa: E402
from indexer.events.mch.builders.nominator import nominator_registries  # noqa: E402
from indexer.events.mch.builders.stonfi_liquidity import stonfi_liquidity_registries  # noqa: E402
from indexer.events.mch.builders.stonfi_provide import stonfi_provide_registries  # noqa: E402
from indexer.events.mch.builders.stonfi_v1 import stonfi_v1_registries  # noqa: E402
from indexer.events.mch.builders.telegram_nft import telegram_nft_registries  # noqa: E402
from indexer.events.mch.builders.tgbtc import tgbtc_registries  # noqa: E402
from indexer.events.mch.builders.tonco import tonco_liquidity_registries  # noqa: E402
from indexer.events.mch.builders.tonco_swap import tonco_swap_registries  # noqa: E402
from indexer.events.mch.builders.tonstakers import tonstakers_registries  # noqa: E402
from indexer.events.mch.builders.vesting import vesting_registries  # noqa: E402

_FACTORIES = [
    coffee_registries, dedust_hybrid_registries, dedust_registries,
    dedust_withdraw_registries, dedust_v2_claim_reward_registries,
    dns_registries, jetton_decl_registries,
    jetton_registries, stonfi_v2_registries, coffee_liquidity_registries,
    dedust_deposit_registries, ethena_registries, evaa_registries,
    getgems_registries, jvault_registries, layerzero_receive_registries,
    nft_transfer_registries,
    nominator_registries, stonfi_liquidity_registries, stonfi_provide_registries,
    stonfi_v1_registries, telegram_nft_registries, tgbtc_registries,
    tonco_liquidity_registries, tonco_swap_registries, tonstakers_registries,
    vesting_registries,
]

_DICTS = ("opcodes", "predicates", "block_types", "builders",
          "message_types", "lookups", "fns", "shapers")


def combined_registries() -> Registries:
    out = Registries()
    for f in _FACTORIES:
        r = f()
        for d in _DICTS:
            getattr(out, d).update(getattr(r, d))
    return out
