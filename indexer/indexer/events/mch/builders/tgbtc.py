"""mch host fns + shaper for the tgBTC family (specs/tgbtc.mch).

HYBRID declarative build (C++-runnable): the matcher has no `build` directive —
the produced btype, the reject decision and the out-field mapping live in the
.mch; the imperative residue is the single host fn `tgbtc_mint_data` below. It
delegates to the shared core `build_tgbtc_mint_core` in blocks/tgbtc.py (the same
function the legacy matcher calls) and exposes the produced TgBTCMintData
dataclass as a namespace. `null` from the core (missing pieces / unparseable log)
drives `reject when d == null`.

Consumed-set-driven (the well-known `consumed` env name), NOT capture-driven —
same situation as specs/evaa.mch: the three blocks the core needs sit in the
recursion's exit branch (@tail/@log/@mint would be list bindings under §7.2 and
the compiled path's positional walker mis-binds same-position exit captures).
Each role carries a unique signature within one match's consumed set:

- head:        the sole CallContractBlock with opcode 0x3F781D24 (also the anchor).
- success log: the sole CallContractBlock with opcode 0x77A80EF3.
- jetton_mint: the sole block with btype "jetton_mint".

Deployment side-effects ride on the shaper `tgbtc_deploy_absorb`: every
CallContractBlock whose transaction deploys its destination carries a
`ContractDeploy` block in its `children_blocks` (attached at init, sharing the
parent's event node — NOT reachable via `next_blocks`, so the structural match
never consumes it). The legacy matcher sweeps these up and merges them into the
produced block; to stay byte-identical (ContractDeploy as a direct child) the
shaper collects them from the consumed set's `children_blocks` and merges them as
the last build step, after the wrapper's consumed-merge. `unwind_deployments`
still surfaces them as their own `contract_deploy` actions.

W2 — the four EVENT-LOG parsers
-------------------------------
The other five matchers in the spec (`tgbtc_burn`, `tgbtc_new_key` and the four
`*_log_only` fallbacks) are otherwise fully declarative; their only imperative
residue is reading one external-out log body. That residue is factored into FOUR
pure parse fns, one per event opcode, each taking the log block and returning a
namespace of its fields (or `null` when the body is unreadable — the caller's
`reject when` turns that into "no match", exactly like the legacy `try_build`
swallowing the parser exception). Each is shared by the full matcher and its
log-only twin, so a burn parsed through the chain and a burn parsed from a naked
log go through the same code:

    tgbtc_mint_log           0x77A80EF3   -> amount, recipient, bitcoin_txid
    tgbtc_burn_log           0xCA444CE6   -> amount, sender, pegout
    tgbtc_new_key_log        0x27756729   -> amount, pubkey, pegout
    tgbtc_dkg_completed_log  0x453443A6   -> timestamp, pubkey

(the last one is not called `tgbtc_dkg_log` only because that name is taken by
the btype the dkg fallback matcher produces.)

They are parse fns and NOTHING else — no tree navigation, no field selection —
so every produced field is mapped in the `.mch` `out{}`.

Why fns and not `mch-msgs.schema` rows: `TgBTCBurnEvent` is deliberately
TOLERANT (`blocks/messages/tgbtc.py:41` wraps both `load_address` calls in
`try/except`, yielding `AccountId(None)` for a truncated body — the
`35MtsOa0…` fixture is exactly that), and the schema's `addr_py` leaf hard-fails
instead, which would reject the whole match. The other three also need the two
hex renderings Python does at the call site (`hex(x)[2:]` min-hex and the
little-endian 32-byte txid), and the expression language has no string ops. One
parse fn per event body keeps both concerns in the one place that already has to
be dual-implemented.
"""
from __future__ import annotations

from dataclasses import fields as dc_fields
from types import SimpleNamespace

from indexer.events.blocks.basic_blocks import CallContractBlock, ContractDeploy
from indexer.events.blocks.core import Block
from indexer.events.blocks.jettons import JettonMintBlock
from indexer.events.blocks.tgbtc import (
    TgBTCMintBlock,
    build_tgbtc_mint_core,
)
from indexer.events.blocks.messages.jettons import JettonBurnNotification
from indexer.events.blocks.messages.tgbtc import (
    TgBTCBurnEvent,
    TgBTCDkgCompletedEvent,
    TgBTCMintEvent,
    TgBTCNewKeyEvent,
)
from indexer.events.mch.registry import Registries

_HEAD_OPCODE = 0x3F781D24
_TAIL_OPCODE = 0xE42A879B

OPCODES = {
    "TgBTCMintHead":       _HEAD_OPCODE,
    "TgBTCMintTail":       _TAIL_OPCODE,
    "TgBTCMintSuccessLog": TgBTCMintEvent.opcode,
    # W2: the rest of the family. The two "notify" heads a burn can arrive
    # through, the new-key request, the shared payout tail, and the three
    # remaining event-log opcodes.
    "JettonBurnNotify":    JettonBurnNotification.opcode,   # 0x7BDD97DE
    "TgBTCBurnNotify":     0x587643A2,
    "TgBTCNewKeyRequest":  0x690F357A,
    "TgBTCPayout":         0xBE44E7A6,
    "TgBTCPayoutNotify":   0x1A84C0E0,
    "TgBTCBurnLog":        TgBTCBurnEvent.opcode,           # 0xCA444CE6
    "TgBTCNewKeyLog":      TgBTCNewKeyEvent.opcode,         # 0x27756729
    "TgBTCDkgCompletedLog": TgBTCDkgCompletedEvent.opcode,  # 0x453443A6
}

BLOCK_TYPES = {
    "TgBTCMintBlock": TgBTCMintBlock,
    "jetton_mint":    JettonMintBlock,
    # Declarative producers: snake_case btypes, generic Block (the IR engine
    # stamps the name straight onto the block it constructs).
    "tgbtc_burn":     Block,
    "tgbtc_new_key":  Block,
    "tgbtc_dkg_log":  Block,
}


def _parse_log(cls: type, log: Block):
    """Parse a log block's body with `cls`, or None if the body is unreadable.

    None is how each parse fn signals "reject", which is what the legacy
    matchers do implicitly: `build_block` calls the parser bare, and a raised
    exception is caught by `BlockMatcher.try_build` and turned into no-match.
    """
    try:
        return cls(log.get_body())
    except Exception:  # noqa: BLE001 - any malformed body is a reject
        return None


def _first_call(consumed: list[Block], opcode: int) -> Block | None:
    return next(
        (b for b in consumed
         if isinstance(b, CallContractBlock) and b.opcode == opcode),
        None,
    )


def register_tgbtc(reg: Registries) -> Registries:
    """Register everything specs/tgbtc.mch needs into `reg`."""
    reg.opcodes.update(OPCODES)
    reg.block_types.update(BLOCK_TYPES)

    @reg.register_fn("tgbtc_mint_data")
    async def tgbtc_mint_data(consumed):
        head = _first_call(consumed, _HEAD_OPCODE)
        success_log = _first_call(consumed, TgBTCMintEvent.opcode)
        jetton_mint = next(
            (b for b in consumed if b.btype == "jetton_mint"), None
        )
        produced = build_tgbtc_mint_core(head, success_log, jetton_mint)
        if produced is None:
            return None
        return SimpleNamespace(
            **{f.name: getattr(produced.data, f.name) for f in dc_fields(produced.data)}
        )

    # --- W2 event-log parse fns (see the module docstring) -------------------
    # `hex(x)[2:]`: Python's minimal-length lowercase hex, no leading zeros.
    # Reproduced verbatim rather than zero-padded to 64 — the legacy fills write
    # it straight into `extra.pubkey` and the yaml cases pin the short form.

    @reg.register_fn("tgbtc_mint_log")
    async def tgbtc_mint_log(log):
        d = _parse_log(TgBTCMintEvent, log)
        if d is None:
            return None
        return SimpleNamespace(
            amount=d.amount,
            recipient=d.recipient_address,
            # int -> 32 LITTLE-endian bytes -> hex (the on-wire bitcoin txid
            # byte order; the misleading legacy local name says big_endian).
            bitcoin_txid=d.bitcoin_txid.to_bytes(32, byteorder="little").hex(),
        )

    @reg.register_fn("tgbtc_burn_log")
    async def tgbtc_burn_log(log):
        d = _parse_log(TgBTCBurnEvent, log)
        if d is None:
            return None
        # Both addresses are ALWAYS an AccountId here — TgBTCBurnEvent swallows a
        # truncated body and yields AccountId(None) (addr_none), never raises.
        return SimpleNamespace(
            amount=d.amount, sender=d.sender_address, pegout=d.pegout_address
        )

    @reg.register_fn("tgbtc_new_key_log")
    async def tgbtc_new_key_log(log):
        d = _parse_log(TgBTCNewKeyEvent, log)
        if d is None:
            return None
        return SimpleNamespace(
            amount=d.amount,
            pubkey=hex(d.new_internal_pubkey)[2:],
            pegout=d.pegout_address,
        )

    @reg.register_fn("tgbtc_dkg_completed_log")
    async def tgbtc_dkg_completed_log(log):
        d = _parse_log(TgBTCDkgCompletedEvent, log)
        if d is None:
            return None
        return SimpleNamespace(
            timestamp=d.timestamp, pubkey=hex(d.internal_pubkey)[2:]
        )

    @reg.register_shaper("tgbtc_deploy_absorb")
    def tgbtc_deploy_absorb(produced: Block, match) -> None:
        deploys = [
            c
            for b in match.consumed
            for c in b.children_blocks
            if isinstance(c, ContractDeploy)
        ]
        if deploys:
            produced.merge_blocks(deploys)

    return reg


def tgbtc_registries() -> Registries:
    """Fresh Registries ready to compile specs/tgbtc.mch."""
    return register_tgbtc(Registries())
