"""mch registration for the whole Cocoon family (specs/cocoon.mch).

specs/cocoon.mch is FULLY DECLARATIVE — twelve matchers, all `out` blocks, no
build cores — so this module registers only the names the spec references: the
eighteen opcodes, the twelve produced btypes, the thirteen body parsers, and the
two host fns the expression language cannot express.

`cocoon_expected_address` — the `expectedMyAddress` field of PayoutPayload /
    LastPayoutPayload / ChargePayload / GrantRefundPayload is an address that is
    routinely EXTERNAL on this protocol, and every legacy build renders it as a
    STRING with two spellings: `f"{len};{hex(value)[2:]}"` for an
    ExternalAddress, `to_str(False).upper()` for a std one, and None left as
    None. String formatting is deliberately outside the language, so this is a
    host fn. The three call sites (worker payout, proxy
    charge, grant refund) are byte-identical in legacy, hence one fn.

`cocoon_withdraw_amount` — a NESTED payload parse: ClientProxyRequest.payload is
    a `maybe_ref` whose cell must then be `begin_parse`d as
    ClientProxyRefundGranted to reach the coins. `parse CAPTURE as T` reads only
    the captured block's own body, so the second hop needs a host fn. It
    reproduces CocoonClientWithdrawMatcher.build_block's try/except verbatim:
    ANY failure (unparseable request body, absent payload, unparseable payload)
    yields None, and the spec's `?? 0` is legacy's "warn and write 0".

The twelve legacy matchers in blocks/cocoon.py are unchanged and remain the
parity reference.
"""
from __future__ import annotations

from typing import Any

from pytoniq_core import Address, ExternalAddress

from indexer.events.blocks.core import Block
from indexer.events.blocks.messages.cocoon import (
    CocoonChargePayload,
    CocoonClientProxyRefundGranted,
    CocoonClientProxyRequest,
    CocoonExtClientTopUp,
    CocoonExtProxyPayoutRequest,
    CocoonGrantRefundPayload,
    CocoonLastPayoutPayload,
    CocoonOwnerClientChangeSecretHash,
    CocoonOwnerClientIncreaseStake,
    CocoonOwnerClientRegister,
    CocoonOwnerClientRequestRefund,
    CocoonOwnerClientWithdraw,
    CocoonOwnerWalletSendMessage,
    CocoonPayout,
    CocoonPayoutPayload,
    CocoonRegisterProxy,
    CocoonReturnExcessesBack,
    CocoonUnregisterProxy,
    CocoonWorkerProxyRequest,
)
from indexer.events.mch.registry import Registries

OPCODES = {
    "CocoonPayoutPayload":               CocoonPayoutPayload.opcode,
    "CocoonLastPayoutPayload":           CocoonLastPayoutPayload.opcode,
    "CocoonWorkerProxyRequest":          CocoonWorkerProxyRequest.opcode,
    "CocoonPayout":                      CocoonPayout.opcode,
    "CocoonReturnExcessesBack":          CocoonReturnExcessesBack.opcode,
    "CocoonExtProxyPayoutRequest":       CocoonExtProxyPayoutRequest.opcode,
    "CocoonChargePayload":               CocoonChargePayload.opcode,
    "CocoonClientProxyRequest":          CocoonClientProxyRequest.opcode,
    "CocoonExtClientTopUp":              CocoonExtClientTopUp.opcode,
    "CocoonRegisterProxy":               CocoonRegisterProxy.opcode,
    "CocoonUnregisterProxy":             CocoonUnregisterProxy.opcode,
    "CocoonOwnerClientRegister":         CocoonOwnerClientRegister.opcode,
    "CocoonOwnerClientChangeSecretHash": CocoonOwnerClientChangeSecretHash.opcode,
    "CocoonOwnerWalletSendMessage":      CocoonOwnerWalletSendMessage.opcode,
    "CocoonOwnerClientRequestRefund":    CocoonOwnerClientRequestRefund.opcode,
    "CocoonGrantRefundPayload":          CocoonGrantRefundPayload.opcode,
    "CocoonOwnerClientIncreaseStake":    CocoonOwnerClientIncreaseStake.opcode,
    "CocoonOwnerClientWithdraw":         CocoonOwnerClientWithdraw.opcode,
}

# Declarative producers: snake_case btypes carried by a generic Block (the IR
# engine stamps the name onto the block it constructs). The legacy
# Cocoon*Block classes stay in blocks/cocoon.py as the parity reference; nothing
# reads their dataclass `data` off an mch-produced block, because the serializer
# fills go through `_dget` (block_tree_serializer.py:99).
BLOCK_TYPES = {
    "cocoon_worker_payout":             Block,
    "cocoon_proxy_payout":              Block,
    "cocoon_proxy_charge":              Block,
    "cocoon_client_top_up":             Block,
    "cocoon_register_proxy":            Block,
    "cocoon_unregister_proxy":          Block,
    "cocoon_client_register":           Block,
    "cocoon_client_change_secret_hash": Block,
    "cocoon_client_request_refund":     Block,
    "cocoon_grant_refund":              Block,
    "cocoon_client_increase_stake":     Block,
    "cocoon_client_withdraw":           Block,
}

MESSAGE_TYPES = {
    "CocoonPayoutPayload":               CocoonPayoutPayload,
    "CocoonLastPayoutPayload":           CocoonLastPayoutPayload,
    "CocoonWorkerProxyRequest":          CocoonWorkerProxyRequest,
    "CocoonExtProxyPayoutRequest":       CocoonExtProxyPayoutRequest,
    "CocoonChargePayload":               CocoonChargePayload,
    "CocoonExtClientTopUp":              CocoonExtClientTopUp,
    "CocoonRegisterProxy":               CocoonRegisterProxy,
    "CocoonUnregisterProxy":             CocoonUnregisterProxy,
    "CocoonOwnerClientRegister":         CocoonOwnerClientRegister,
    "CocoonOwnerClientChangeSecretHash": CocoonOwnerClientChangeSecretHash,
    "CocoonOwnerClientRequestRefund":    CocoonOwnerClientRequestRefund,
    "CocoonGrantRefundPayload":          CocoonGrantRefundPayload,
    "CocoonOwnerClientIncreaseStake":    CocoonOwnerClientIncreaseStake,
    "CocoonOwnerClientWithdraw":         CocoonOwnerClientWithdraw,
}


async def _cocoon_expected_address_fn(addr: Any) -> str | None:
    """`expected_address`: the legacy string rendering of an expectedMyAddress.

    Verbatim from the three build bodies in blocks/cocoon.py (:134, :373, :1170).
    An ExternalAddress renders as "<bit-length>;<lowercase hex, no leading
    zeros>"; a std Address as its raw uppercase form; anything else (addr_none,
    i.e. a pytoniq None) stays None.
    """
    if isinstance(addr, ExternalAddress):
        return f"{addr.len};{hex(addr.external_address)[2:]}"
    if isinstance(addr, Address):
        return addr.to_str(False).upper()
    return None


async def _cocoon_withdraw_amount_fn(client_request: Any) -> int | None:
    """`withdraw_amount`: the coins inside ClientProxyRequest.payload.

    CocoonClientWithdrawMatcher.build_block (blocks/cocoon.py:1066-1077) with its
    try/except intact — a missing block, an unparseable request body, an absent
    payload or an unparseable payload all yield None, and the spec's `?? 0`
    supplies the legacy fallback.
    """
    if client_request is None:
        return None
    try:
        request = CocoonClientProxyRequest(client_request.get_body())
        if not request.payload:
            return None
        return CocoonClientProxyRefundGranted(request.payload.begin_parse()).coins
    except Exception:  # noqa: BLE001 - legacy catches everything here
        return None


def register_cocoon(reg: Registries) -> Registries:
    """Register everything specs/cocoon.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)
    reg.message_types.update(MESSAGE_TYPES)
    reg.register_fn("cocoon_expected_address")(_cocoon_expected_address_fn)
    reg.register_fn("cocoon_withdraw_amount")(_cocoon_withdraw_amount_fn)

    return reg


def cocoon_registries() -> Registries:
    """Fresh Registries ready to compile specs/cocoon.mch."""
    return register_cocoon(Registries())
