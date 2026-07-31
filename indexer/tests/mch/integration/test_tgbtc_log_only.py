"""Legacy-vs-IR parity for the two tgBTC log-only matchers with NO corpus fixture.

`tgbtc_burn_log_only` and `tgbtc_dkg_log_only` each have a real `.lz4`
(`35MtsOa0…`, `_6GHAnnp8…`) and are covered by the parity sweep. The other two —
`tgbtc_mint_log_only` and `tgbtc_new_key_log_only` — need a trace where the full
mint / new-key chain is ABSENT but its external-out log survives, and no such
trace has been recorded. The sweep still runs them on the six mint/new_key
fixtures as reject-identical rows (the full matcher must claim the log first),
which pins the negative path but never the positive one.

So the positive path is built here the way test_dns.py does it: real
Transaction/Message DB models carrying a genuine event BOC -> to_tree() ->
init_block(), then the legacy matcher and the IR matcher run over two
independently constructed trees and their products are compared field by field.

The naked-log shape is exactly what the fallbacks exist for: one external-out
message (no destination) hanging off an ordinary call, with nothing else of the
protocol in reach.
"""
from __future__ import annotations

from pathlib import Path

import pytest
from pytoniq_core import Address, begin_cell

from indexer.core.database import Message, MessageContent, Transaction
from indexer.events.blocks.core import Block
from indexer.events.blocks.tgbtc import (
    TgBTCMintLogOnlyMatcher,
    TgBTCNewKeyLogOnlyMatcher,
)
from indexer.events.blocks.utils.tree_utils import to_tree
from indexer.events.event_processing import init_block
from indexer.events.mch import compile_file
from indexer.events.mch.builders import tgbtc as tgbtc_builders
import indexer.events.mch.specs as specs_pkg
from indexer.events.mch.ir_emit import emit_ir

TGBTC_MCH = Path(specs_pkg.__file__).parent / "tgbtc.mch"

try:
    from indexer.events import mch_ir as _mch_ir

    _HAVE_IR = hasattr(_mch_ir, "load_artifact") and hasattr(_mch_ir, "build_matchers")
except Exception:  # pragma: no cover - import error path
    _mch_ir = None
    _HAVE_IR = False

MINT_LOG_OPCODE = 0x77A80EF3
NEW_KEY_LOG_OPCODE = 0x27756729
CARRIER_OPCODE = 0x11111111

TELEPORT = Address("0:" + "a1" * 32)
RECIPIENT = Address("0:" + "b2" * 32)
PEGOUT = Address("0:" + "c3" * 32)
CALLER = Address("0:" + "d4" * 32)

TXID = int.from_bytes(bytes(range(32)), "big")
PUBKEY = 0x382E9D762046F4BB5CDE2CE1B7780CB499B5394972163930CF88BC55463ACB70


def _mint_log_body() -> bytes:
    return (begin_cell().store_uint(MINT_LOG_OPCODE, 32).store_coins(9850)
            .store_address(RECIPIENT).store_uint(TXID, 256).end_cell()).to_boc()


def _new_key_log_body() -> bytes:
    return (begin_cell().store_uint(NEW_KEY_LOG_OPCODE, 32).store_coins(425489053588)
            .store_uint(PUBKEY, 256).store_address(PEGOUT).end_cell()).to_boc()


def _carrier_body() -> bytes:
    return begin_cell().store_uint(CARRIER_OPCODE, 32).end_cell().to_boc()


def _tx(hash_: str, lt: int, account: str) -> Transaction:
    tx = Transaction(
        hash=hash_, lt=lt, account=account, now=1000 + lt, total_fees=0,
        orig_status="active", end_status="active", descr="ord",
        aborted=False, emulated=False,
    )
    tx.messages = []
    return tx


def _msg(msg_hash, tx, direction, opcode, src, dst, body, value=10 ** 9) -> Message:
    m = Message(
        msg_hash=msg_hash, tx_hash=tx.hash, tx_lt=tx.lt, direction=direction,
        source=src, destination=dst, value=value, fwd_fee=0, ihr_fee=0,
        created_lt=tx.lt, created_at=tx.now, opcode=opcode, bounce=False,
        bounced=False, value_extra_currencies=None,
    )
    m.message_content = MessageContent(hash="c", body=body)
    m.transaction = tx
    return m


def _naked_log_transactions(log_opcode: int, log_body: bytes) -> list[Transaction]:
    """A caller tx forwarding an opaque call to the teleport contract, whose tx
    emits ONLY the event log as an external-out message (no destination)."""
    caller = CALLER.to_str(False)
    teleport = TELEPORT.to_str(False)

    root_tx = _tx("TXROOT", 100, caller)
    tele_tx = _tx("TXTELE", 200, teleport)

    root_tx.messages = [
        _msg("EXT", root_tx, "in", None, None, caller, _carrier_body(), value=None),
        _msg("MCALL", root_tx, "out", CARRIER_OPCODE, caller, teleport, _carrier_body()),
    ]
    tele_tx.messages = [
        _msg("MCALL", tele_tx, "in", CARRIER_OPCODE, caller, teleport, _carrier_body()),
        # external-out: destination None is what makes this a log leaf
        _msg("MLOG", tele_tx, "out", log_opcode, teleport, None, log_body, value=None),
    ]
    return [root_tx, tele_tx]


def _ir_matchers():
    if not _HAVE_IR:
        pytest.skip("indexer.events.mch_ir not importable; the IR engine is the DSL runtime")
    registries = tgbtc_builders.tgbtc_registries()
    artifact = emit_ir(compile_file(TGBTC_MCH, registries), [TGBTC_MCH.name])
    return list(_mch_ir.build_matchers(_mch_ir.load_artifact(artifact), registries))


async def _run(matchers, txs) -> list[Block]:
    root = Block("root", [])
    root.connect(init_block(to_tree(txs)))
    out: list[Block] = []
    for m in matchers:
        for b in root.bfs_iter():
            if b.btype == "root" or b.parent is not None:
                continue
            r = await m.try_build(b)
            if r is not None:
                out.extend(x for x in r if x.parent is None)
    return out


def _data(block: Block) -> dict:
    d = block.data
    return d if isinstance(d, dict) else vars(d)


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "name,legacy_cls,log_opcode,body_fn,btype",
    [
        ("mint", TgBTCMintLogOnlyMatcher, MINT_LOG_OPCODE, _mint_log_body, "tgbtc_mint"),
        ("new_key", TgBTCNewKeyLogOnlyMatcher, NEW_KEY_LOG_OPCODE, _new_key_log_body,
         "tgbtc_new_key"),
    ],
)
async def test_log_only_parity(name, legacy_cls, log_opcode, body_fn, btype):
    txs = _naked_log_transactions(log_opcode, body_fn())

    legacy = await _run([legacy_cls()], txs)
    ir = [b for b in await _run(_ir_matchers(), txs) if b.btype == btype]

    assert len(legacy) == 1, f"legacy {name} log-only produced {len(legacy)} blocks"
    assert len(ir) == 1, f"ir {name} log-only produced {len(ir)} blocks"

    ld, idata = _data(legacy[0]), _data(ir[0])
    assert legacy[0].btype == ir[0].btype == btype
    assert set(ld) == set(idata), (
        f"data keys: legacy-only={sorted(set(ld) - set(idata))} "
        f"ir-only={sorted(set(idata) - set(ld))}"
    )
    for key in sorted(ld):
        assert repr(ld[key]) == repr(idata[key]), f"data[{key!r}]"
    assert ld["crippled"] is True
    # single-node match: the log leaf and nothing else
    assert sorted(n.message.tx_hash for n in legacy[0].event_nodes) == \
        sorted(n.message.tx_hash for n in ir[0].event_nodes)
    assert len(ir[0].event_nodes) == 1
